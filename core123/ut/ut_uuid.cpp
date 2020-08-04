#include <core123/uuid.hpp>
#include <core123/ut.hpp>
#include <core123/scanint.hpp>

int main(int, char **){
    auto u = core123::gen_random_uuid();
    std::cout << u << "\n";
    CHECK(u[14] == '4'); // Version 4
    CHECK(u[19] == '8' || u[19]=='9' || u[19] == 'a' || u[19] == 'b'); // Variant 1
    EQUAL(u.size(), 36);

    auto v = core123::gen_random_uuid();
    std::cout << v << "\n";
    CHECK(v[14] == '4');  // Version 4
    CHECK(v[19] == '8' || v[19]=='9' || v[19] == 'a' || v[19] == 'b'); // Variant 1
    EQUAL(v.size(), 36);

    // Check that the values scan as hex, that hyphens are in the
    // right place, and that at least 40 bits changed between u and v.
    int sum = 0;
    uint64_t ui, vi;
    using core123::scanint;
    size_t offu = 0;
    size_t offv = 0;
    // Hyphens 8   13   18   23
    // 01234567-9012-4567-9012-456789012345
    offu = scanint<uint64_t, 16, false>(u, &ui, offu);
    offv = scanint<uint64_t, 16, false>(v, &vi, offv);
    EQUAL(offu, 8); EQUAL(offv,8);
    EQUAL(u[offu], '-'); EQUAL(v[offv], '-');
    sum += core123::popcnt(ui^vi);

    offu = scanint<uint64_t, 16, false>(u, &ui, offu+1);
    offv = scanint<uint64_t, 16, false>(v, &vi, offv+1);
    EQUAL(offu, 13); EQUAL(offv,13);
    EQUAL(u[offu], '-'); EQUAL(v[offv], '-');
    sum += core123::popcnt(ui^vi);
    
    offu = scanint<uint64_t, 16, false>(u, &ui, offu+1);
    offv = scanint<uint64_t, 16, false>(v, &vi, offv+1);
    EQUAL(offu, 18); EQUAL(offv,18);
    EQUAL(u[offu], '-'); EQUAL(v[offv], '-');
    sum += core123::popcnt(ui^vi);

    offu = scanint<uint64_t, 16, false>(u, &ui, offu+1);
    offv = scanint<uint64_t, 16, false>(v, &vi, offv+1);
    EQUAL(offu, 23); EQUAL(offv,23);
    EQUAL(u[offu], '-'); EQUAL(v[offv], '-');
    sum += core123::popcnt(ui^vi);

    offu = scanint<uint64_t, 16, false>(u, &ui, offu+1);
    offv = scanint<uint64_t, 16, false>(v, &vi, offv+1);
    EQUAL(offu, 36); EQUAL(offv,36);
    EQUAL(u[offu], '\0'); EQUAL(v[offv], '\0');
    sum += core123::popcnt(ui^vi);

    CHECK(sum > 40);

    return utstatus();
}
