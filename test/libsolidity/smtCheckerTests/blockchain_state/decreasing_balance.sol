pragma experimental SMTChecker;

contract C {
	uint t;
	constructor() {
		t = address(this).balance;
	}
	function f(address payable a, uint x) public {
		require(address(this).balance >= x);
		a.transfer(x);
	}
	function inv() public view {
		// If only looking at `f`, it looks like this.balance always decreases.
		// However, the edge case of a contract `selfdestruct` sending its remaining balance
		// to this contract should make the claim false (since there's no fallback/receive here).
		assert(address(this).balance == t);
	}
}
// ----
// Warning 6328: (496-530): CHC: Assertion violation happens here.
