#ifndef LIMIT_ORDERBOOK_CPP
#define LIMIT_ORDERBOOK_CPP

#include <cstdint>
#include <unordered_map>
#include <map>

using OrderId = uint64_t;
using Price = uint64_t;
using Quantity = uint64_t;

enum class Side {
    BUY,
    SELL
};

enum class OrderAttributes {
    DEFAULT,
    FULFILL_ALL
};

struct Party {
    OrderId id;
    Quantity quantity;
    Price price;
};

struct Transaction {
    std::vector<Party> buyers, sellers;
    Price bid_ask_spread;
};

struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
    OrderAttributes attributes;
    
    // doubly-linked list ptrs for constant removal
    Order* prev = nullptr;
    Order* next = nullptr;
    Order(OrderId id, Side side, Price price, Quantity qty, OrderAttributes attr = OrderAttributes::DEFAULT, Order* prev = nullptr, Order* next = nullptr):
        id(id), side(side), price(price), quantity(qty), attributes(attr), prev(prev), next(next) {}
};

struct PriceLevel {
    Price price;
    Quantity total_volume = 0;
    
    // Queue pointers
    Order* head = nullptr;
    Order* tail = nullptr;

    PriceLevel(Price price, Quantity vol, Order* head = nullptr, Order* tail = nullptr):
        price(price), total_volume(vol), head(head), tail(tail) {}

    void append_order(Order* order) { // add an order to the linkedlist
        total_volume += order -> quantity;
        order -> prev = tail;
        order -> next = nullptr;
        if (tail != nullptr) {
            tail -> next = order;
        } else {
            head = order;
        }
        tail = order;
    }
    void remove_order(Order* order) { // remove an order (e.g., cancellation, fulfilment)
        total_volume -= order -> quantity;
        if (order -> prev) {
            order -> prev -> next = order -> next;
        } else {
            head = order -> next;
        }
        if (order -> next) {
            order -> next -> prev = order -> prev;
        } else {
            tail = order -> prev;
        }
        order -> prev = order -> next = nullptr;
    }
};

class LimitOrderBook {
private:
    std::unordered_map<OrderId, Order*> order_map; // full unordered hashmap access
    std::map<Price, PriceLevel, std::greater<Price>> bids; // forward iterator
    std::map<Price, PriceLevel, std::greater<Price>> asks; // reverse iterator

    
    void add_to_book(OrderId id, Side side, Price price, Quantity qty, OrderAttributes attr) {
        Order* order = new Order(id, side, price, qty, attr, nullptr, nullptr);
        order_map.emplace(id, order); // add to hashmap for constant-time search
        auto& target = (side == Side::BUY) ? bids : asks;
        auto [iterator, inserted] = target.try_emplace(price, PriceLevel(price, 0, nullptr, nullptr)); // construct empty PriceLevel if DNE; otherwise do nothing
        iterator -> second.append_order(order); // add this order to a linked list corresponding to price ties
    }

    void remove_from_book(Order* order) {
        if (order -> side == Side::BUY) {
            auto loc = bids.find(order -> price); // find the order in bids
            if (loc != bids.end()) { // if found
                PriceLevel& level = loc -> second;
                level.remove_order(order); // remove the order
                if (level.head == nullptr) {
                    bids.erase(loc); // if the PriceLevel is now empty (no bids at this price), then erase it entirely
                }
            }
        } else {
            auto loc = asks.find(order -> price); // find the order in asks
            if (loc != asks.end()) { // if found
                PriceLevel& level = loc -> second;
                level.remove_order(order); // remove the order
                if (level.head == nullptr) {
                    asks.erase(loc); // if the PriceLevel is now empty (no asks at this price), then erase it entirely
                }
            }
        }
        order_map.erase(order -> id); // remove the order from the unordered hashmap search base
        delete order; // free memory
    }

public:
    LimitOrderBook() = default;

    ~LimitOrderBook() {
        // safely delete all allocated Order pointers
        for (auto [id, order] : order_map) {
            delete order;
        }
        order_map.clear();
    }

    Transaction process_order(OrderId id, Side side, Price price, Quantity qty, OrderAttributes attr = OrderAttributes::DEFAULT) {
        auto& match_map = (side == Side::BUY) ? asks : bids; // buy orders match with asks; sell orders mask with bids
        if (match_map.empty()) { // no opposing liquidity, shelve the order
            add_to_book(id, side, price, qty, attr);
            return Transaction();
        }
        auto top_match_level = (side == Side::BUY) ? std::prev(match_map.end()) : match_map.begin(); // reverse iteration for buy (lowest seller), forward for sell (highest buyer)

        // count total fulfillable volume
        Transaction trades; // track using Transactions
        auto& this_side = (side == Side::BUY) ? trades.buyers : trades.sellers;
        auto& other_side = (side == Side::BUY) ? trades.sellers : trades.buyers;
        this_side.push_back(Party{id, qty, price});
        auto iter = top_match_level;
        Quantity total_volume = 0;
        uint64_t remaining = match_map.size();
        while (remaining-- > 0) {
            PriceLevel& level = (*iter).second;
            (side == Side::BUY) ? (--iter) : (++iter);
            if ((side == Side::BUY) ? (level.price > price) : (level.price < price)) { // all further orders are outside the acceptable range
                break;
            }

            // otherwise, we continue
            // iterate through the PriceLevel- must do this anyway to get the order quantities
            Order* order = level.head;
            while (order != nullptr) {
                if (total_volume + order -> quantity <= qty) { // execute full
                    other_side.push_back(Party{order -> id, order -> quantity, order -> price});
                    total_volume += order -> quantity;
                } else { // execute partial
                    other_side.push_back(Party{order -> id, qty - total_volume, order -> price});
                    total_volume = qty;
                    break; // done fulfilling
                }
                order = order -> next;
            }
            if (total_volume >= qty) {
                break;
            }
        }

        // order fulfilment/insertion logic
        if (attr == OrderAttributes::FULFILL_ALL && total_volume < qty) { // order cannot be fulfilled. all or nothing -> nothing. reject and throw an exception
            throw std::domain_error("Order could not be fulfilled with full volume at requested price.");
        }
        // otherwise, fulfill the order- either the order is DEFAULT, or total available volume is enough for FULFILL_ALL order, leaving zero unfulfilled volume
        for (auto party : other_side) {
            Order* order = order_map[party.id];
            if (party.quantity == order -> quantity) { // this order was fulfilled. pop
                remove_from_book(order);
            } else { // this order was not fulfilled. modify amount
                order -> quantity -= party.quantity;
                match_map[order -> price].total_volume -= party.quantity;
            }
        }
        if (total_volume < qty) {
            add_to_book(id, side, price, qty - total_volume, attr);
        }
        return trades;
    }

    void cancel_order(OrderId id) {
        auto loc = order_map.find(id);
        if (loc == order_map.end()) {
            throw std::domain_error("This order ID does not exist.");
        }
        remove_from_book((*loc).second);
        return;
    }

    /**
     * @brief Modifies the quantity of an existing order.
     * 
     * Rule: If quantity decreases, it keeps its place in the time queue.
     * If quantity increases, it loses its time priority and moves to the back (tail).
     * 
     * @param id The OrderId to modify
     * @param new_qty The new desired quantity
     */
    void modify_order(OrderId id, Quantity new_qty) {
        auto it = order_map.find(id);
        if (it == order_map.end()) {
            throw std::domain_error("This order ID does not exist.");
        }
        Order& order = *(it -> second);
        if (new_qty == order.quantity) {
            return; // no change needed
        }
        auto& book = (order.side == Side::BUY) ? bids : asks;
        auto level_it = book.find(order.price);
        if (level_it == book.end()) {
            throw std::logic_error("This should not have happened."); // should theoretically never happen in a consistent book
        }

        PriceLevel& level = level_it->second;
        if (new_qty < order.quantity) { // maintain time priority
            Quantity diff = order.quantity - new_qty;
            order.quantity = new_qty;
            level.total_volume -= diff;
        } else { // lose time priority (move to back of list)
            if (level.tail == &order) {
                Quantity diff = new_qty - order.quantity;
                order.quantity = new_qty;
                level.total_volume += diff;
            } else {
                level.remove_order(&order); // this one doesn't use delete so it's fine to keep using order ptr
                order.quantity = new_qty;
                level.append_order(&order); // reattach at the end
            }
        }
    }


    // --- Analytics / getters (useful for testing) ---
    Quantity get_best_bid_volume() const;
    Quantity get_best_ask_volume() const;
    Price get_best_bid() const;
    Price get_best_ask() const;
};

#endif

#include <iostream>

int main() {
    std::cout << "Hello World!" << std::endl;
    return 0;
}