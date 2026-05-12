// hello.lang — entry point that imports mathlib
import "std.io";
import "std.string";
import "mathlib";

class Stack {
    private int[] data;
    private int   top;

    public fun void init(int capacity) {
        this.data = new int[capacity];
        this.top  = 0;
    }

    public fun void push(int val) {
        this.data[this.top] = val;
        this.top = this.top + 1;
    }

    public fun int pop() {
        this.top = this.top - 1;
        return this.data[this.top];
    }

    public fun int peek() {
        return this.data[this.top - 1];
    }

    public fun int size() {
        return this.top;
    }

    public fun bool isEmpty() {
        return this.top == 0;
    }
}

fun int main() {
    println("=== Multi-file compilation demo ===");
    println("");

    println("-- mathlib: factorial --");
    int i = 0;
    while (i <= 7) {
        try {
            int f = factorial(i);
            print(toString(i));
            print("! = ");
            println(toString(f));
        } catch (Exception e) {
            println("Error: " + e);
        }
        i = i + 1;
    }

    println("");
    println("-- mathlib: gcd --");
    print("gcd(48, 18) = ");
    println(toString(gcd(48, 18)));
    print("gcd(100, 75) = ");
    println(toString(gcd(100, 75)));

    println("");
    println("-- mathlib: primes up to 30 --");
    int n = 2;
    while (n <= 30) {
        if (isPrime(n)) {
            print(toString(n));
            print(" ");
        }
        n = n + 1;
    }
    println("");

    println("");
    println("-- mathlib: sumArray --");
    int[] vals = {10, 20, 30, 40, 50};
    print("sum = ");
    println(toString(sumArray(vals)));

    println("");
    println("-- Stack class --");
    Stack s = new Stack(10);
    s.push(1);
    s.push(2);
    s.push(3);
    s.push(4);
    s.push(5);
    print("size = ");
    println(toString(s.size()));
    print("peek = ");
    println(toString(s.peek()));
    print("pop  = ");
    println(toString(s.pop()));
    print("pop  = ");
    println(toString(s.pop()));
    print("size after 2 pops = ");
    println(toString(s.size()));

    println("");
    println("-- Closure: make adder --");
    int offset = 100;
    var adder = (int x) => x + offset;
    println("adder is a closure capturing offset=100");

    println("");
    println("Done.");
    return 0;
}
