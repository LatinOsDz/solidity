pragma experimental SMTChecker;
contract C {
    function f() public pure returns (byte) {
        byte a = 0xff;
        byte b = 0xf0;
        b |= a;
        assert(a == b);

        a |= ~b;
        assert(a == 0); // fails
    }
}
// ----
// Warning 6321: (83-87): Unnamed return parameter can remain uninitialized.
// Warning 6328: (203-217): CHC: Assertion violation happens here.
