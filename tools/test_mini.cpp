// Unit test for launcher/mini.h (the launcher's own small vector/string/map/set/optional/function).
// Build and run it with the sanitizers, from the repo root (any clang; zig c++ works):
//   clang++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-exceptions -fno-rtti \
//       -nostdinc++ -Ilauncher tools/test_mini.cpp -o /tmp/test_mini && /tmp/test_mini
// Each case printed "ok"; a failed CHECK prints its line and the run exits 1. ASan reports any bad memory access.
#include "mini.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(x) do { if (!(x)) { printf("  FAILED line %d: %s\n", __LINE__, #x); fails++; } } while (0)

struct Counted {                    // counts live objects so a leak or a double destroy shows up
    static int live;
    int v;
    Counted(int x = 0) : v(x) { live++; }
    Counted(const Counted &o) : v(o.v) { live++; }
    Counted(Counted &&o) : v(o.v) { o.v = -1; live++; }
    Counted &operator=(const Counted &o) { v = o.v; return *this; }
    Counted &operator=(Counted &&o) { v = o.v; o.v = -1; return *this; }
    ~Counted() { live--; }
    bool operator<(const Counted &o) const { return v < o.v; }
    bool operator==(const Counted &o) const { return v == o.v; }
};
int Counted::live = 0;

static void test_vector() {
    {
        mini::vector<Counted> a;
        for (int i = 0; i < 100; i++) a.push_back(Counted(i));
        CHECK(a.size() == 100 && a[57].v == 57);
        for (int i = 0; i < 50; i++) a.push_back(a[0]);          // an element of itself, across regrowth
        CHECK(a.size() == 150 && a[149].v == 0);
        mini::vector<Counted> b = a;                             // copy
        CHECK(b.size() == 150 && b[10].v == 10);
        b = b;                                                   // self-assign
        CHECK(b.size() == 150 && b[99].v == 99);
        b = mini::move(b);                                       // self-move
        CHECK(b.size() == 150);
        mini::vector<Counted> c = mini::move(a);
        CHECK(a.empty() && c.size() == 150);
        c.insert(c.begin(), Counted(-5));
        c.insert(c.end(), Counted(-6));
        c.insert(c.begin() + 3, c[1]);                           // an element of itself
        CHECK(c[0].v == -5 && c[3].v == 0 && c.back().v == -6 && c.size() == 153);
        c.erase(c.begin(), c.begin() + 3);
        CHECK(c[0].v == 0 && c.size() == 150);
        c.erase(c.end() - 1);
        c.erase(c.begin() + 5, c.begin() + 5);
        CHECK(c.size() == 149);
        c.erase(c.begin(), c.end());
        CHECK(c.empty());
        c.resize(10, Counted(7));
        c.resize(3);
        CHECK(c.size() == 3 && c[2].v == 7);
        c.emplace_back(9);
        CHECK(c.back().v == 9);
        c.pop_back();
        mini::vector<Counted> d{Counted(1), Counted(2)};
        d.swap(c);
        CHECK(d.size() == 3 && c.size() == 2);
        d = {Counted(4)};
        CHECK(d.size() == 1 && d[0].v == 4);
        mini::vector<int> e(5);
        CHECK(e.size() == 5 && e[4] == 0);
        CHECK(mini::find(e.begin(), e.end(), 3) == e.end());
        e[2] = 3;
        CHECK(mini::find(e.begin(), e.end(), 3) == e.begin() + 2);
        e.erase(mini::remove_if(e.begin(), e.end(), [](int x) { return x == 0; }), e.end());
        CHECK(e.size() == 1 && e[0] == 3);
        mini::vector<mini::vector<mini::string>> nest;
        for (int i = 0; i < 20; i++) nest.push_back({mini::to_string(i), "x"});
        nest.push_back(nest[3]);
        CHECK(nest.back()[0] == "3");
    }
    CHECK(Counted::live == 0);
}

static void test_string() {
    mini::string e;
    CHECK(e.empty() && e.c_str()[0] == 0 && e == "" && e.find("x") == mini::string::npos && e.rfind("") == 0);
    CHECK(e.find("") == 0 && e.find_first_of("ab") == mini::string::npos && e.find_last_not_of(" ") == mini::string::npos);
    mini::string s = "hello world";
    s = s.c_str() + 6;                                           // assign from itself
    CHECK(s == "world");
    s.append(s.c_str(), 3);                                      // append from itself
    CHECK(s == "worldwor");
    s += s;
    CHECK(s == "worldworworldwor");
    for (int i = 0; i < 10; i++) s += s.c_str() + s.size() - 1; // grows while reading itself
    CHECK(s.size() == 26 && s.back() == 'r');
    mini::string t = "a=b=c";
    CHECK(t.find('=') == 1 && t.rfind('=') == 3 && t.find('=', 2) == 3 && t.rfind('=', 2) == 1 && t.rfind('a') == 0);
    CHECK(t.find("c", 5) == mini::string::npos && t.find("c", 99) == mini::string::npos && t.rfind("zzzzzzz") == mini::string::npos);
    CHECK(t.find("", 5) == 5 && t.find("", 6) == mini::string::npos && t.rfind("", 2) == 2);
    CHECK(t.substr(2) == "b=c" && t.substr(9) == "" && t.substr(1, 99) == "=b=c");
    t.insert(0, "X").insert(t.size(), "Y").replace(1, 1, "AA");
    CHECK(t == "XAA=b=cY");
    t.erase(0, 1).erase(t.size() - 1);
    CHECK(t == "AA=b=c");
    t.erase(99);
    CHECK(t == "AA=b=c");
    mini::string w = "  pad  ";
    CHECK(w.find_first_not_of(" ") == 2 && w.find_last_not_of(" ") == 4 && w.find_last_of("p") == 2);
    CHECK(mini::string("abc") < mini::string("abd") && mini::string("ab") < mini::string("abc") && !(e < e));
    CHECK(("x" + mini::string("y") + 'z') == "xyz" && mini::to_string(-12) == "-12");
    mini::string m = mini::move(s);
    CHECK(s.empty() && s.c_str()[0] == 0 && m.size() == 26);
    m = mini::move(m);
    CHECK(m.size() == 26);
    mini::string n(3, 'q');
    CHECK(n == "qqq");
    mini::wstring ws = L"wide";
    CHECK(ws.size() == 4 && ws == L"wide");
}

static void test_map_set() {
    {
        mini::map<mini::string, Counted> m;
        const char *k[] = {"m", "a", "z", "c", "a", "q"};
        for (int i = 0; i < 6; i++) m[k[i]] = Counted(i);
        CHECK(m.size() == 5 && m["a"].v == 4 && m.begin()->first == "a" && (m.end() - 1)->first == "z");
        CHECK(m.count("c") == 1 && m.count("nope") == 0 && m.find("nope") == m.end());
        CHECK(m.erase("c") == 1 && m.erase("c") == 0 && m.size() == 4);
        mini::map<mini::string, Counted> c = m;
        m.clear();
        CHECK(c.at("q").v == 5 && m.empty());
        for (int i = 0; i < 300; i++) c[mini::to_string(i)] = Counted(i);
        for (int i = 0; i < 300; i += 2) c.erase(mini::to_string(i));
        CHECK(c.size() == 154 && c.at("299").v == 299);
        mini::set<int> s;
        CHECK(s.insert(3) && s.insert(1) && !s.insert(3) && s.insert(2));
        CHECK(s.size() == 3 && *s.begin() == 1 && s.count(2) && !s.count(9));
        CHECK(s.erase(2) == 1 && s.erase(2) == 0 && s.size() == 2);
    }
    CHECK(Counted::live == 0);
}

static void test_optional() {
    {
        mini::optional<Counted> a;
        CHECK(!a && a == mini::nullopt);
        a = Counted(3);
        CHECK(a && a->v == 3);
        mini::optional<Counted> b = a;                           // non-const lvalue: the copy, not the U&& ctor
        CHECK(b && b->v == 3);
        b = b;
        CHECK(b && b->v == 3);
        mini::optional<Counted> c = mini::move(b);
        CHECK(c->v == 3);
        c = mini::nullopt;
        CHECK(!c);
        c.emplace(8);
        CHECK(c.value().v == 8 && a != c);
        mini::optional<mini::string> s = "text";                 // the converting ctor
        CHECK(s && *s == "text" && mini::optional<mini::string>().value_or("d") == "d");
        mini::optional<mini::string> s2 = s;
        CHECK(s2 == s);
    }
    CHECK(Counted::live == 0);
}

static int hits = 0;
static void bump() { hits++; }

static void test_function() {
    {
        Counted held(5);
        mini::function<int(int)> f = [held](int x) { return x + held.v; };
        mini::function<int(int)> g = f;                          // non-const lvalue copy: crashed before 0016
        CHECK(f(1) == 6 && g(2) == 7);
        auto h = g;                                              // what the launcher's dialogs did
        CHECK(h(0) == 5);
        const mini::function<int(int)> &cr = h;
        mini::function<int(int)> i = cr;
        CHECK(i(1) == 6);
        mini::function<int(int)> j = mini::move(i);
        CHECK(!i && j(2) == 7);
        j = j;                                                   // self-assign
        CHECK(j(3) == 8);
        j = mini::move(j);                                       // self-move
        CHECK(j && j(3) == 8);
        j = f;
        j = nullptr;
        CHECK(!j);
        mini::function<int(int)> empty, e2 = empty, e3;
        e3 = empty;
        CHECK(!empty && !e2 && !e3);
        mini::vector<mini::function<int(int)>> v;                // copied and moved inside a growing vector
        for (int k = 0; k < 40; k++) v.push_back([k](int x) { return x * k; });
        v.push_back(v[7]);
        mini::vector<mini::function<int(int)>> v2 = v;
        CHECK(v2[39](2) == 78 && v2.back()(3) == 21);
        mini::function<void()> p = bump;                         // plain function pointer
        mini::function<void()> q = p;
        q();
        p();
        CHECK(hits == 2);
        const auto lam = [held] { return held.v; };
        mini::function<int()> fromconst = lam;                   // a const lambda
        CHECK(fromconst() == 5);
        mini::function<mini::string(const mini::string &)> sf = [](const mini::string &a) { return a + "!"; };
        CHECK(sf("hi") == "hi!");
        mini::function<void()> nest = [f, g]() mutable { hits += f(0) + g(0); };
        mini::function<void()> nest2 = nest;
        nest2();
        CHECK(hits == 12);
    }
    CHECK(Counted::live == 0);
}

int main() {
    struct { const char *name; void (*fn)(); } cases[] = {
        {"vector", test_vector}, {"string", test_string}, {"map/set", test_map_set},
        {"optional", test_optional}, {"function", test_function},
    };
    for (auto &c : cases) {
        int before = fails;
        c.fn();
        printf("%s %s\n", fails == before ? "ok   " : "FAIL ", c.name);
    }
    printf(fails ? "%d checks failed\n" : "all mini.h checks passed\n", fails);
    return fails ? 1 : 0;
}
