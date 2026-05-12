// mathlib.lang — reusable math utilities
// Import with:  import "mathlib";

fun int factorial(int n) throws {
    if (n < 0) {
        throw "factorial: negative input";
    }
    if (n == 0 || n == 1) {
        return 1;
    }
    int result = 1;
    int i = 2;
    while (i <= n) {
        result = result * i;
        i = i + 1;
    }
    return result;
}

fun int gcd(int a, int b) {
    while (b != 0) {
        int tmp = b;
        b = a - (a / b) * b;
        a = tmp;
    }
    return a;
}

fun bool isPrime(int n) {
    if (n < 2) {
        return false;
    }
    int i = 2;
    while (i * i <= n) {
        if (n - (n / i) * i == 0) {
            return false;
        }
        i = i + 1;
    }
    return true;
}

fun int sumArray(int[] arr) {
    int total = 0;
    int i = 0;
    while (i < arr.length) {
        total = total + arr[i];
        i = i + 1;
    }
    return total;
}
