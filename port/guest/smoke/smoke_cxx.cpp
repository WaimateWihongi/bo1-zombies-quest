// C++ checks for the smoke test: static constructors, virtual calls, new/delete, local statics.
struct Shape {
    virtual ~Shape() {}
    virtual int area() const = 0;
};

struct Square : Shape {
    int side;
    explicit Square(int s) : side(s) {}
    int area() const override { return side * side; }
};

extern "C" int smoke_seed(void);

static int constructed;

struct Global {
    Global() { constructed = smoke_seed(); }
};

static Global global;

static int counted()
{
    static int calls = constructed + 1;
    return calls;
}

extern "C" int smoke_cxx(void)
{
    Shape *s = new Square(7);
    int ok = s->area() == 49;
    delete s;
    int *array = new int[100];
    array[99] = 5;
    ok &= array[99] == 5;
    delete[] array;
    return ok && constructed == 1234 && counted() == 1235;
}
