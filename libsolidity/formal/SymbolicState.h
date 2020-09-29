/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <libsolidity/formal/SymbolicTypes.h>
#include <libsolidity/formal/SymbolicVariables.h>

#include <libsmtutil/Sorts.h>
#include <libsmtutil/SolverInterface.h>

namespace solidity::frontend::smt
{

class EncodingContext;
class SymbolicAddressVariable;
class SymbolicArrayVariable;

class BlockchainVariable
{
public:
	BlockchainVariable(std::string _name, std::map<std::string, smtutil::SortPointer> _members, EncodingContext& _context);
	/// @returns the variable data as a tuple.
	smtutil::Expression value() { return m_tuple->currentValue(); }
	smtutil::Expression value(unsigned _idx) { return m_tuple->valueAtIndex(_idx); }
	smtutil::SortPointer sort() { return m_tuple->sort(); }
	unsigned index() { return m_tuple->index(); }
	void newVar() { m_tuple->increaseIndex(); }
	void reset() { m_tuple->resetIndex(); }

	/// @returns the symbolic _member.
	smtutil::Expression member(std::string const& _member);
	/// Generates a new tuple where _member is assigned _value.
	smtutil::Expression assignMember(std::string const& _member, smtutil::Expression const& _value);

private:
	std::string m_name;
	std::map<std::string, smtutil::SortPointer> m_members;
	EncodingContext& m_context;
	std::map<std::string, unsigned> m_componentIndices;
	std::unique_ptr<SymbolicTupleVariable> m_tuple;
};

/**
 * Symbolic representation of the blockchain context:
 * - error flag
 * - this (the address of the currently executing contract)
 * - state, represented as a tuple of:
 *   - balances
 *   - TODO: potentially storage of contracts
 * - block and transaction properties, represented as a tuple of:
 *   - blockhash
 *   - block coinbase
 *   - block difficulty
 *   - block gaslimit
 *   - block number
 *   - block timestamp
 *   - TODO gasleft
 *   - msg data
 *   - msg sender
 *   - msg sig
 *   - msg value
 *   - tx gasprice
 *   - tx origin
 */
class SymbolicState
{
public:
	SymbolicState(EncodingContext& _context): m_context(_context) {}

	void reset();

	/// Error flag.
	//@{
	SymbolicIntVariable& errorFlag() { return m_error; }
	smtutil::SortPointer errorFlagSort() { return m_error.sort(); }
	//@}

	/// This.
	//@{
	/// @returns the symbolic value of the currently executing contract's address.
	smtutil::Expression thisAddress() { return m_thisAddress.currentValue(); }
	smtutil::Expression thisAddress(unsigned _idx) { return m_thisAddress.valueAtIndex(_idx); }
	smtutil::SortPointer thisAddressSort() { return m_thisAddress.sort(); }
	//@}

	/// Blockchain state.
	//@{
	smtutil::Expression state() { return m_state.value(); }
	smtutil::Expression state(unsigned _idx) { return m_state.value(_idx); }
	smtutil::SortPointer stateSort() { return m_state.sort(); }
	void newState() { m_state.newVar(); }
	/// @returns the symbolic balances.
	smtutil::Expression balances();
	/// @returns the symbolic balance of address `this`.
	smtutil::Expression balance();
	/// @returns the symbolic balance of an address.
	smtutil::Expression balance(smtutil::Expression _address);

	/// Transfer _value from _from to _to.
	void transfer(smtutil::Expression _from, smtutil::Expression _to, smtutil::Expression _value);
	//@}

	/// Transaction data.
	//@{
	/// @returns the tx data as a tuple.
	smtutil::Expression tx() { return m_tx.value(); }
	smtutil::Expression tx(unsigned _idx) { return m_tx.value(_idx); }
	smtutil::SortPointer txSort() { return m_tx.sort(); }
	void newTx() { m_tx.newVar(); }
	smtutil::Expression txMember(std::string const& _member);
	void addTxConstraints(FunctionDefinition const& _function);
	smtutil::Expression blockhash(smtutil::Expression _blockNumber);
	//@}

private:
	/// Adds _value to _account's balance.
	void addBalance(smtutil::Expression _account, smtutil::Expression _value);

	EncodingContext& m_context;

	SymbolicIntVariable m_error{
		TypeProvider::uint256(),
		TypeProvider::uint256(),
		"error",
		m_context
	};

	SymbolicAddressVariable m_thisAddress{
		"this",
		m_context
	};

	BlockchainVariable m_state{
		"state",
		{{"balances", std::make_shared<smtutil::ArraySort>(smtutil::SortProvider::uintSort, smtutil::SortProvider::uintSort)}},
		m_context
	};

	BlockchainVariable m_tx{
		"tx",
		{
			{"blockhash", std::make_shared<smtutil::ArraySort>(smtutil::SortProvider::uintSort, smtutil::SortProvider::uintSort)},
			{"block.coinbase", smt::smtSort(*TypeProvider::address())},
			{"block.difficulty", smtutil::SortProvider::uintSort},
			{"block.gaslimit", smtutil::SortProvider::uintSort},
			{"block.number", smtutil::SortProvider::uintSort},
			{"block.timestamp", smtutil::SortProvider::uintSort},
			// TODO gasleft
			{"msg.data", smt::smtSort(*TypeProvider::bytesMemory())},
			{"msg.sender", smt::smtSort(*TypeProvider::address())},
			{"msg.sig", smtutil::SortProvider::uintSort},
			{"msg.value", smtutil::SortProvider::uintSort},
			{"tx.gasprice", smtutil::SortProvider::uintSort},
			{"tx.origin", smtutil::SortProvider::uintSort}
		},
		m_context
	};
};

}
