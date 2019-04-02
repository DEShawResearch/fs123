#include "core123/producerconsumerqueue.hpp"
#include <utility>
#include <thread>
#include <iostream>

using core123::producerconsumerqueue;
using upq_t = producerconsumerqueue<std::unique_ptr<int>>;

std::mutex coutmtx;

void up_producer(upq_t& q){
    for(int i=0; i<10; ++i){
        {std::lock_guard<std::mutex> lg{coutmtx};
            std::cout << "up_produce: " << i << std::endl;}
        q.emplace( new int(i) );
    }
    q.close();
}

void up_consumer(upq_t& q){
    upq_t::value_type next;
    while(q.dequeue(next)){
        {std::lock_guard<std::mutex> lg{coutmtx};
            std::cout << "up_consume: " << *next << std::endl;}
    }
}

using iq_t = producerconsumerqueue<int>;

void i_producer(iq_t* q){
    for(int i=0; i<10; ++i){
        {std::lock_guard<std::mutex> lg{coutmtx};
            std::cout << "i_produce: " << i << std::endl;}
        q->enqueue( i );
    }
    q->close();
}

void i_consumer(iq_t* q){
    iq_t::value_type next;
    while(q->dequeue(next)){
        {std::lock_guard<std::mutex> lg{coutmtx};
            std::cout << "i_consume: " << next << std::endl;}
    }
}

int main(int, char **){
    iq_t iq;
    auto tic = std::thread(i_consumer, &iq);
    auto tip = std::thread(i_producer, &iq);

    upq_t upq;
    auto tc = std::thread(up_consumer, std::ref(upq));
    auto tp = std::thread(up_producer, std::ref(upq));


    tc.join();
    tp.join();
    tic.join();
    tip.join();
    return 0;
}


    
