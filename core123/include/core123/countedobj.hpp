#pragma once
// A mix-in class that allows ups to keep track of
// the number of live objects.

// Usage:
//     unsigned long myObjCtr;
//
//     struct myObj : private countedobj<myObj, decltype(myObjCtr)>{
//            ...
//     };
//     DECLARE_COUNTER(myObj, myObjCtr);
//
// myObjCtr always tells you how many 'myObj's there
// are.
//
// If myObj might be created/destroyed in multiple threads,
// then the counter should be std:atomic, e.g.,
//
//    std::atomic<unsigned long> myObjCtr;

namespace core123{
template <typename CountedType, typename CounterType>
struct countedobj{
    countedobj() { cnt++; }
    countedobj(const countedobj&) : countedobj() {}
    countedobj(countedobj&&) : countedobj(){}
    countedobj& operator=(const countedobj&){}
    countedobj& operator=(countedobj&&){}
    ~countedobj(){ cnt--; }
private:
    static CounterType& cnt;
};

#define countedobjDECLARATION(T, Ctr) \
    template<> decltype(Ctr)& countedobj<T, decltype(Ctr)>::cnt = Ctr

} // namespace core123

