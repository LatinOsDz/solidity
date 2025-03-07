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

#include <libsolidity/formal/CHC.h>

#ifdef HAVE_Z3
#include <libsmtutil/Z3CHCInterface.h>
#endif

#include <libsolidity/formal/ArraySlicePredicate.h>
#include <libsolidity/formal/PredicateInstance.h>
#include <libsolidity/formal/PredicateSort.h>
#include <libsolidity/formal/SymbolicTypes.h>

#include <libsolidity/ast/TypeProvider.h>

#include <libsmtutil/CHCSmtLib2Interface.h>
#include <libsolutil/Algorithms.h>

#include <boost/range/adaptor/reversed.hpp>

#include <queue>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::langutil;
using namespace solidity::smtutil;
using namespace solidity::frontend;
using namespace solidity::frontend::smt;

CHC::CHC(
	EncodingContext& _context,
	ErrorReporter& _errorReporter,
	[[maybe_unused]] map<util::h256, string> const& _smtlib2Responses,
	[[maybe_unused]] ReadCallback::Callback const& _smtCallback,
	SMTSolverChoice _enabledSolvers
):
	SMTEncoder(_context),
	m_outerErrorReporter(_errorReporter),
	m_enabledSolvers(_enabledSolvers)
{
	bool usesZ3 = _enabledSolvers.z3;
#ifndef HAVE_Z3
	usesZ3 = false;
#endif
	if (!usesZ3)
		m_interface = make_unique<CHCSmtLib2Interface>(_smtlib2Responses, _smtCallback);
}

void CHC::analyze(SourceUnit const& _source)
{
	solAssert(_source.annotation().experimentalFeatures.count(ExperimentalFeature::SMTChecker), "");

	resetSourceAnalysis();

	set<SourceUnit const*, IdCompare> sources;
	sources.insert(&_source);
	for (auto const& source: _source.referencedSourceUnits(true))
		sources.insert(source);
	for (auto const* source: sources)
		defineInterfacesAndSummaries(*source);
	for (auto const* source: sources)
		source->accept(*this);

	checkVerificationTargets();
}

vector<string> CHC::unhandledQueries() const
{
	if (auto smtlib2 = dynamic_cast<CHCSmtLib2Interface const*>(m_interface.get()))
		return smtlib2->unhandledQueries();

	return {};
}

bool CHC::visit(ContractDefinition const& _contract)
{
	resetContractAnalysis();

	initContract(_contract);

	m_stateVariables = SMTEncoder::stateVariablesIncludingInheritedAndPrivate(_contract);

	clearIndices(&_contract);

	solAssert(m_currentContract, "");
	m_constructorSummaryPredicate = createSymbolicBlock(
		constructorSort(*m_currentContract, state()),
		"summary_constructor_" + contractSuffix(_contract),
		PredicateType::ConstructorSummary,
		&_contract
	);

	SMTEncoder::visit(_contract);
	return false;
}

void CHC::endVisit(ContractDefinition const& _contract)
{
	auto implicitConstructorPredicate = createSymbolicBlock(
		implicitConstructorSort(state()),
		"implicit_constructor_" + contractSuffix(_contract),
		PredicateType::ImplicitConstructor,
		&_contract
	);
	addRule(
		(*implicitConstructorPredicate)({0, state().thisAddress(), state().state()}),
		implicitConstructorPredicate->functor().name
	);
	setCurrentBlock(*implicitConstructorPredicate);

	if (auto constructor = _contract.constructor())
		constructor->accept(*this);
	else
		inlineConstructorHierarchy(_contract);

	connectBlocks(m_currentBlock, summary(_contract));

	setCurrentBlock(*m_constructorSummaryPredicate);

	addAssertVerificationTarget(m_currentContract, m_currentBlock, smtutil::Expression(true), errorFlag().currentValue());
	connectBlocks(m_currentBlock, interface(), errorFlag().currentValue() == 0);

	SMTEncoder::endVisit(_contract);
}

bool CHC::visit(FunctionDefinition const& _function)
{
	if (!_function.isImplemented())
	{
		addRule(summary(_function), "summary_function_" + to_string(_function.id()));
		return false;
	}

	// This is the case for base constructor inlining.
	if (m_currentFunction)
	{
		solAssert(m_currentFunction->isConstructor(), "");
		solAssert(_function.isConstructor(), "");
		solAssert(_function.scope() != m_currentContract, "");
		SMTEncoder::visit(_function);
		return false;
	}

	solAssert(!m_currentFunction, "Function inlining should not happen in CHC.");
	m_currentFunction = &_function;

	initFunction(_function);

	auto functionEntryBlock = createBlock(m_currentFunction, PredicateType::FunctionEntry);
	auto bodyBlock = createBlock(&m_currentFunction->body(), PredicateType::FunctionBlock);

	auto functionPred = predicate(*functionEntryBlock);
	auto bodyPred = predicate(*bodyBlock);

	if (_function.isConstructor())
		connectBlocks(m_currentBlock, functionPred);
	else
		addRule(functionPred, functionPred.name);

	m_context.addAssertion(errorFlag().currentValue() == 0);
	for (auto const* var: m_stateVariables)
		m_context.addAssertion(m_context.variable(*var)->valueAtIndex(0) == currentValue(*var));
	for (auto const& var: _function.parameters())
		m_context.addAssertion(m_context.variable(*var)->valueAtIndex(0) == currentValue(*var));
	m_context.addAssertion(state().state(0) == state().state());

	connectBlocks(functionPred, bodyPred);

	setCurrentBlock(*bodyBlock);

	SMTEncoder::visit(*m_currentFunction);

	return false;
}

void CHC::endVisit(FunctionDefinition const& _function)
{
	if (!_function.isImplemented())
		return;

	solAssert(m_currentFunction && m_currentContract, "");

	// This is the case for base constructor inlining.
	if (m_currentFunction != &_function)
	{
		solAssert(m_currentFunction && m_currentFunction->isConstructor(), "");
		solAssert(_function.isConstructor(), "");
		solAssert(_function.scope() != m_currentContract, "");
	}
	else
	{
		// We create an extra exit block for constructors that simply
		// connects to the interface in case an explicit constructor
		// exists in the hierarchy.
		// It is not connected directly here, as normal functions are,
		// because of the case where there are only implicit constructors.
		// This is done in endVisit(ContractDefinition).
		if (_function.isConstructor())
		{
			string suffix = m_currentContract->name() + "_" + to_string(m_currentContract->id());
			solAssert(m_currentContract, "");
			auto constructorExit = createSymbolicBlock(
				constructorSort(*m_currentContract, state()),
				"constructor_exit_" + suffix,
				PredicateType::ConstructorSummary,
				m_currentContract
			);
			connectBlocks(m_currentBlock, predicate(*constructorExit));

			setCurrentBlock(*constructorExit);
		}
		else
		{
			auto assertionError = errorFlag().currentValue();
			auto sum = summary(_function);
			connectBlocks(m_currentBlock, sum);

			auto iface = interface();

			setCurrentBlock(*m_interfaces.at(m_currentContract));

			auto ifacePre = smt::interfacePre(*m_interfaces.at(m_currentContract), *m_currentContract, m_context);
			if (_function.isPublic())
			{
				addAssertVerificationTarget(&_function, ifacePre, sum, assertionError);
				connectBlocks(ifacePre, iface, sum && (assertionError == 0));
			}
		}
		m_currentFunction = nullptr;
	}

	SMTEncoder::endVisit(_function);
}

bool CHC::visit(IfStatement const& _if)
{
	solAssert(m_currentFunction, "");

	bool unknownFunctionCallWasSeen = m_unknownFunctionCallSeen;
	m_unknownFunctionCallSeen = false;

	solAssert(m_currentFunction, "");
	auto const& functionBody = m_currentFunction->body();

	auto ifHeaderBlock = createBlock(&_if, PredicateType::FunctionBlock, "if_header_");
	auto trueBlock = createBlock(&_if.trueStatement(), PredicateType::FunctionBlock, "if_true_");
	auto falseBlock = _if.falseStatement() ? createBlock(_if.falseStatement(), PredicateType::FunctionBlock, "if_false_") : nullptr;
	auto afterIfBlock = createBlock(&functionBody, PredicateType::FunctionBlock);

	connectBlocks(m_currentBlock, predicate(*ifHeaderBlock));

	setCurrentBlock(*ifHeaderBlock);
	_if.condition().accept(*this);
	auto condition = expr(_if.condition());

	connectBlocks(m_currentBlock, predicate(*trueBlock), condition);
	if (_if.falseStatement())
		connectBlocks(m_currentBlock, predicate(*falseBlock), !condition);
	else
		connectBlocks(m_currentBlock, predicate(*afterIfBlock), !condition);

	setCurrentBlock(*trueBlock);
	_if.trueStatement().accept(*this);
	connectBlocks(m_currentBlock, predicate(*afterIfBlock));

	if (_if.falseStatement())
	{
		setCurrentBlock(*falseBlock);
		_if.falseStatement()->accept(*this);
		connectBlocks(m_currentBlock, predicate(*afterIfBlock));
	}

	setCurrentBlock(*afterIfBlock);

	if (m_unknownFunctionCallSeen)
		eraseKnowledge();

	m_unknownFunctionCallSeen = unknownFunctionCallWasSeen;

	return false;
}

bool CHC::visit(WhileStatement const& _while)
{
	bool unknownFunctionCallWasSeen = m_unknownFunctionCallSeen;
	m_unknownFunctionCallSeen = false;

	solAssert(m_currentFunction, "");
	auto const& functionBody = m_currentFunction->body();

	auto namePrefix = string(_while.isDoWhile() ? "do_" : "") + "while";
	auto loopHeaderBlock = createBlock(&_while, PredicateType::FunctionBlock, namePrefix + "_header_");
	auto loopBodyBlock = createBlock(&_while.body(), PredicateType::FunctionBlock, namePrefix + "_body_");
	auto afterLoopBlock = createBlock(&functionBody, PredicateType::FunctionBlock);

	auto outerBreakDest = m_breakDest;
	auto outerContinueDest = m_continueDest;
	m_breakDest = afterLoopBlock;
	m_continueDest = loopHeaderBlock;

	if (_while.isDoWhile())
		_while.body().accept(*this);

	connectBlocks(m_currentBlock, predicate(*loopHeaderBlock));

	setCurrentBlock(*loopHeaderBlock);

	_while.condition().accept(*this);
	auto condition = expr(_while.condition());

	connectBlocks(m_currentBlock, predicate(*loopBodyBlock), condition);
	connectBlocks(m_currentBlock, predicate(*afterLoopBlock), !condition);

	// Loop body visit.
	setCurrentBlock(*loopBodyBlock);
	_while.body().accept(*this);

	m_breakDest = outerBreakDest;
	m_continueDest = outerContinueDest;

	// Back edge.
	connectBlocks(m_currentBlock, predicate(*loopHeaderBlock));
	setCurrentBlock(*afterLoopBlock);

	if (m_unknownFunctionCallSeen)
		eraseKnowledge();

	m_unknownFunctionCallSeen = unknownFunctionCallWasSeen;

	return false;
}

bool CHC::visit(ForStatement const& _for)
{
	bool unknownFunctionCallWasSeen = m_unknownFunctionCallSeen;
	m_unknownFunctionCallSeen = false;

	solAssert(m_currentFunction, "");
	auto const& functionBody = m_currentFunction->body();

	auto loopHeaderBlock = createBlock(&_for, PredicateType::FunctionBlock, "for_header_");
	auto loopBodyBlock = createBlock(&_for.body(), PredicateType::FunctionBlock, "for_body_");
	auto afterLoopBlock = createBlock(&functionBody, PredicateType::FunctionBlock);
	auto postLoop = _for.loopExpression();
	auto postLoopBlock = postLoop ? createBlock(postLoop, PredicateType::FunctionBlock, "for_post_") : nullptr;

	auto outerBreakDest = m_breakDest;
	auto outerContinueDest = m_continueDest;
	m_breakDest = afterLoopBlock;
	m_continueDest = postLoop ? postLoopBlock : loopHeaderBlock;

	if (auto init = _for.initializationExpression())
		init->accept(*this);

	connectBlocks(m_currentBlock, predicate(*loopHeaderBlock));
	setCurrentBlock(*loopHeaderBlock);

	auto condition = smtutil::Expression(true);
	if (auto forCondition = _for.condition())
	{
		forCondition->accept(*this);
		condition = expr(*forCondition);
	}

	connectBlocks(m_currentBlock, predicate(*loopBodyBlock), condition);
	connectBlocks(m_currentBlock, predicate(*afterLoopBlock), !condition);

	// Loop body visit.
	setCurrentBlock(*loopBodyBlock);
	_for.body().accept(*this);

	if (postLoop)
	{
		connectBlocks(m_currentBlock, predicate(*postLoopBlock));
		setCurrentBlock(*postLoopBlock);
		postLoop->accept(*this);
	}

	m_breakDest = outerBreakDest;
	m_continueDest = outerContinueDest;

	// Back edge.
	connectBlocks(m_currentBlock, predicate(*loopHeaderBlock));
	setCurrentBlock(*afterLoopBlock);

	if (m_unknownFunctionCallSeen)
		eraseKnowledge();

	m_unknownFunctionCallSeen = unknownFunctionCallWasSeen;

	return false;
}

void CHC::endVisit(FunctionCall const& _funCall)
{
	auto functionCallKind = *_funCall.annotation().kind;

	if (functionCallKind != FunctionCallKind::FunctionCall)
	{
		SMTEncoder::endVisit(_funCall);
		return;
	}

	FunctionType const& funType = dynamic_cast<FunctionType const&>(*_funCall.expression().annotation().type);
	switch (funType.kind())
	{
	case FunctionType::Kind::Assert:
		visitAssert(_funCall);
		SMTEncoder::endVisit(_funCall);
		break;
	case FunctionType::Kind::Internal:
		internalFunctionCall(_funCall);
		break;
	case FunctionType::Kind::External:
	case FunctionType::Kind::BareStaticCall:
		externalFunctionCall(_funCall);
		SMTEncoder::endVisit(_funCall);
		break;
	case FunctionType::Kind::DelegateCall:
	case FunctionType::Kind::BareCall:
	case FunctionType::Kind::BareCallCode:
	case FunctionType::Kind::BareDelegateCall:
	case FunctionType::Kind::Creation:
		SMTEncoder::endVisit(_funCall);
		unknownFunctionCall(_funCall);
		break;
	case FunctionType::Kind::KECCAK256:
	case FunctionType::Kind::ECRecover:
	case FunctionType::Kind::SHA256:
	case FunctionType::Kind::RIPEMD160:
	case FunctionType::Kind::BlockHash:
	case FunctionType::Kind::AddMod:
	case FunctionType::Kind::MulMod:
		[[fallthrough]];
	default:
		SMTEncoder::endVisit(_funCall);
		break;
	}

	createReturnedExpressions(_funCall);
}

void CHC::endVisit(Break const& _break)
{
	solAssert(m_breakDest, "");
	connectBlocks(m_currentBlock, predicate(*m_breakDest));
	auto breakGhost = createBlock(&_break, PredicateType::FunctionBlock, "break_ghost_");
	m_currentBlock = predicate(*breakGhost);
}

void CHC::endVisit(Continue const& _continue)
{
	solAssert(m_continueDest, "");
	connectBlocks(m_currentBlock, predicate(*m_continueDest));
	auto continueGhost = createBlock(&_continue, PredicateType::FunctionBlock, "continue_ghost_");
	m_currentBlock = predicate(*continueGhost);
}

void CHC::endVisit(IndexRangeAccess const& _range)
{
	createExpr(_range);

	auto baseArray = dynamic_pointer_cast<SymbolicArrayVariable>(m_context.expression(_range.baseExpression()));
	auto sliceArray = dynamic_pointer_cast<SymbolicArrayVariable>(m_context.expression(_range));
	solAssert(baseArray && sliceArray, "");

	auto const& sliceData = ArraySlicePredicate::create(sliceArray->sort(), m_context);
	if (!sliceData.first)
	{
		for (auto pred: sliceData.second.predicates)
			m_interface->registerRelation(pred->functor());
		for (auto const& rule: sliceData.second.rules)
			addRule(rule, "");
	}

	auto start = _range.startExpression() ? expr(*_range.startExpression()) : 0;
	auto end = _range.endExpression() ? expr(*_range.endExpression()) : baseArray->length();
	auto slicePred = (*sliceData.second.predicates.at(0))({
		baseArray->elements(),
		sliceArray->elements(),
		start,
		end
	});

	m_context.addAssertion(slicePred);
	m_context.addAssertion(sliceArray->length() == end - start);
}

void CHC::visitAssert(FunctionCall const& _funCall)
{
	auto const& args = _funCall.arguments();
	solAssert(args.size() == 1, "");
	solAssert(args.front()->annotation().type->category() == Type::Category::Bool, "");

	solAssert(m_currentContract, "");
	solAssert(m_currentFunction, "");
	if (m_currentFunction->isConstructor())
		m_functionAssertions[m_currentContract].insert(&_funCall);
	else
		m_functionAssertions[m_currentFunction].insert(&_funCall);

	auto previousError = errorFlag().currentValue();
	errorFlag().increaseIndex();

	connectBlocks(
		m_currentBlock,
		m_currentFunction->isConstructor() ? summary(*m_currentContract) : summary(*m_currentFunction),
		currentPathConditions() && !m_context.expression(*args.front())->currentValue() && (
			errorFlag().currentValue() == newErrorId(_funCall)
		)
	);

	m_context.addAssertion(errorFlag().currentValue() == previousError);
}

void CHC::visitAddMulMod(FunctionCall const& _funCall)
{
	auto previousError = errorFlag().currentValue();
	errorFlag().increaseIndex();

	addVerificationTarget(
		&_funCall,
		VerificationTarget::Type::DivByZero,
		errorFlag().currentValue()
	);

	solAssert(_funCall.arguments().at(2), "");
	smtutil::Expression target = expr(*_funCall.arguments().at(2)) == 0 && errorFlag().currentValue() == newErrorId(_funCall);
	m_context.addAssertion((errorFlag().currentValue() == previousError) || target);

	SMTEncoder::visitAddMulMod(_funCall);
}

void CHC::internalFunctionCall(FunctionCall const& _funCall)
{
	solAssert(m_currentContract, "");

	auto const* function = functionCallToDefinition(_funCall);
	if (function)
	{
		if (m_currentFunction && !m_currentFunction->isConstructor())
			m_callGraph[m_currentFunction].insert(function);
		else
			m_callGraph[m_currentContract].insert(function);
		auto const* contract = function->annotation().contract;

		// Libraries can have constants as their "state" variables,
		// so we need to ensure they were constructed correctly.
		if (contract->isLibrary())
			m_context.addAssertion(interface(*contract));
	}

	auto previousError = errorFlag().currentValue();

	m_context.addAssertion(predicate(_funCall));

	connectBlocks(
		m_currentBlock,
		(m_currentFunction && !m_currentFunction->isConstructor()) ? summary(*m_currentFunction) : summary(*m_currentContract),
		(errorFlag().currentValue() > 0)
	);
	m_context.addAssertion(errorFlag().currentValue() == 0);
	errorFlag().increaseIndex();
	m_context.addAssertion(errorFlag().currentValue() == previousError);
}

void CHC::externalFunctionCall(FunctionCall const& _funCall)
{
	/// In external function calls we do not add a "predicate call"
	/// because we do not trust their function body anyway,
	/// so we just add the nondet_interface predicate.

	solAssert(m_currentContract, "");

	FunctionType const& funType = dynamic_cast<FunctionType const&>(*_funCall.expression().annotation().type);
	auto kind = funType.kind();
	solAssert(kind == FunctionType::Kind::External || kind == FunctionType::Kind::BareStaticCall, "");

	auto const* function = functionCallToDefinition(_funCall);
	if (!function)
		return;

	for (auto var: function->returnParameters())
		m_context.variable(*var)->increaseIndex();

	auto preCallState = vector<smtutil::Expression>{state().state()} + currentStateVariables();
	bool usesStaticCall = kind == FunctionType::Kind::BareStaticCall ||
		function->stateMutability() == StateMutability::Pure ||
		function->stateMutability() == StateMutability::View;
	if (!usesStaticCall)
	{
		state().newState();
		for (auto const* var: m_stateVariables)
			m_context.variable(*var)->increaseIndex();
	}

	auto postCallState = vector<smtutil::Expression>{state().state()} + currentStateVariables();
	auto nondet = (*m_nondetInterfaces.at(m_currentContract))(preCallState + postCallState);
	// TODO this could instead add the summary of the called function, where that summary
	// basically has the nondet interface of this summary as a constraint.
	m_context.addAssertion(nondet);

	m_context.addAssertion(errorFlag().currentValue() == 0);
}

void CHC::unknownFunctionCall(FunctionCall const&)
{
	/// Function calls are not handled at the moment,
	/// so always erase knowledge.
	/// TODO remove when function calls get predicates/blocks.
	eraseKnowledge();

	/// Used to erase outer scope knowledge in loops and ifs.
	/// TODO remove when function calls get predicates/blocks.
	m_unknownFunctionCallSeen = true;
}

void CHC::makeArrayPopVerificationTarget(FunctionCall const& _arrayPop)
{
	FunctionType const& funType = dynamic_cast<FunctionType const&>(*_arrayPop.expression().annotation().type);
	solAssert(funType.kind() == FunctionType::Kind::ArrayPop, "");

	auto memberAccess = dynamic_cast<MemberAccess const*>(&_arrayPop.expression());
	solAssert(memberAccess, "");
	auto symbArray = dynamic_pointer_cast<SymbolicArrayVariable>(m_context.expression(memberAccess->expression()));
	solAssert(symbArray, "");

	auto previousError = errorFlag().currentValue();
	errorFlag().increaseIndex();

	addVerificationTarget(&_arrayPop, VerificationTarget::Type::PopEmptyArray, errorFlag().currentValue());

	smtutil::Expression target = (symbArray->length() <= 0) && (errorFlag().currentValue() == newErrorId(_arrayPop));
	m_context.addAssertion((errorFlag().currentValue() == previousError) || target);
}

pair<smtutil::Expression, smtutil::Expression> CHC::arithmeticOperation(
	Token _op,
	smtutil::Expression const& _left,
	smtutil::Expression const& _right,
	TypePointer const& _commonType,
	frontend::Expression const& _expression
)
{
	auto values = SMTEncoder::arithmeticOperation(_op, _left, _right, _commonType, _expression);

	IntegerType const* intType = nullptr;
	if (auto const* type = dynamic_cast<IntegerType const*>(_commonType))
		intType = type;
	else
		intType = TypeProvider::uint256();

	// Mod does not need underflow/overflow checks.
	// Div only needs overflow check for signed types.
	if (_op == Token::Mod || (_op == Token::Div && !intType->isSigned()))
		return values;

	auto previousError = errorFlag().currentValue();
	errorFlag().increaseIndex();

	VerificationTarget::Type targetType;
	unsigned errorId = newErrorId(_expression);

	optional<smtutil::Expression> target;
	if (_op == Token::Div)
	{
		targetType = VerificationTarget::Type::Overflow;
		target = values.second > intType->maxValue() && errorFlag().currentValue() == errorId;
	}
	else if (intType->isSigned())
	{
		unsigned secondErrorId = newErrorId(_expression);
		targetType = VerificationTarget::Type::UnderOverflow;
		target = (values.second < intType->minValue() && errorFlag().currentValue() == errorId) ||
			(values.second > intType->maxValue() && errorFlag().currentValue() == secondErrorId);
	}
	else if (_op == Token::Sub)
	{
		targetType = VerificationTarget::Type::Underflow;
		target = values.second < intType->minValue() && errorFlag().currentValue() == errorId;
	}
	else if (_op == Token::Add || _op == Token::Mul)
	{
		targetType = VerificationTarget::Type::Overflow;
		target = values.second > intType->maxValue() && errorFlag().currentValue() == errorId;
	}
	else
		solAssert(false, "");

	addVerificationTarget(
		&_expression,
		targetType,
		errorFlag().currentValue()
	);

	m_context.addAssertion((errorFlag().currentValue() == previousError) || *target);

	return values;
}

void CHC::resetSourceAnalysis()
{
	m_verificationTargets.clear();
	m_safeTargets.clear();
	m_unsafeTargets.clear();
	m_functionAssertions.clear();
	m_errorIds.clear();
	m_callGraph.clear();
	m_summaries.clear();
	m_interfaces.clear();
	m_nondetInterfaces.clear();
	Predicate::reset();
	ArraySlicePredicate::reset();
	m_blockCounter = 0;

	bool usesZ3 = false;
#ifdef HAVE_Z3
	usesZ3 = m_enabledSolvers.z3;
	if (usesZ3)
	{
		/// z3::fixedpoint does not have a reset mechanism, so we need to create another.
		m_interface.reset(new Z3CHCInterface());
		auto z3Interface = dynamic_cast<Z3CHCInterface const*>(m_interface.get());
		solAssert(z3Interface, "");
		m_context.setSolver(z3Interface->z3Interface());
	}
#endif
	if (!usesZ3)
	{
		auto smtlib2Interface = dynamic_cast<CHCSmtLib2Interface*>(m_interface.get());
		smtlib2Interface->reset();
		solAssert(smtlib2Interface, "");
		m_context.setSolver(smtlib2Interface->smtlib2Interface());
	}

	m_context.clear();
	m_context.setAssertionAccumulation(false);
}

void CHC::resetContractAnalysis()
{
	m_stateVariables.clear();
	m_unknownFunctionCallSeen = false;
	m_breakDest = nullptr;
	m_continueDest = nullptr;
	errorFlag().resetIndex();
}

void CHC::eraseKnowledge()
{
	resetStateVariables();
	m_context.resetVariables([&](VariableDeclaration const& _variable) { return _variable.hasReferenceOrMappingType(); });
}

void CHC::clearIndices(ContractDefinition const* _contract, FunctionDefinition const* _function)
{
	SMTEncoder::clearIndices(_contract, _function);
	for (auto const* var: m_stateVariables)
		/// SSA index 0 is reserved for state variables at the beginning
		/// of the current transaction.
		m_context.variable(*var)->increaseIndex();
	if (_function)
	{
		for (auto const& var: _function->parameters() + _function->returnParameters())
			m_context.variable(*var)->increaseIndex();
		for (auto const& var: _function->localVariables())
			m_context.variable(*var)->increaseIndex();
	}

	state().newState();
}

void CHC::setCurrentBlock(Predicate const& _block)
{
	if (m_context.solverStackHeigh() > 0)
		m_context.popSolver();
	solAssert(m_currentContract, "");
	clearIndices(m_currentContract, m_currentFunction);
	m_context.pushSolver();
	m_currentBlock = predicate(_block);
}

set<frontend::Expression const*, CHC::IdCompare> CHC::transactionAssertions(ASTNode const* _txRoot)
{
	set<Expression const*, IdCompare> assertions;
	solidity::util::BreadthFirstSearch<ASTNode const*>{{_txRoot}}.run([&](auto const* function, auto&& _addChild) {
		assertions.insert(m_functionAssertions[function].begin(), m_functionAssertions[function].end());
		for (auto const* called: m_callGraph[function])
		_addChild(called);
	});
	return assertions;
}

SortPointer CHC::sort(FunctionDefinition const& _function)
{
	return functionSort(_function, m_currentContract, state());
}

SortPointer CHC::sort(ASTNode const* _node)
{
	if (auto funDef = dynamic_cast<FunctionDefinition const*>(_node))
		return sort(*funDef);

	solAssert(m_currentFunction, "");
	return functionBodySort(*m_currentFunction, m_currentContract, state());
}

Predicate const* CHC::createSymbolicBlock(SortPointer _sort, string const& _name, PredicateType _predType, ASTNode const* _node)
{
	auto const* block = Predicate::create(_sort, _name, _predType, m_context, _node);
	m_interface->registerRelation(block->functor());
	return block;
}

void CHC::defineInterfacesAndSummaries(SourceUnit const& _source)
{
	for (auto const& node: _source.nodes())
		if (auto const* contract = dynamic_cast<ContractDefinition const*>(node.get()))
		{
			string suffix = contract->name() + "_" + to_string(contract->id());
			m_interfaces[contract] = createSymbolicBlock(interfaceSort(*contract, state()), "interface_" + suffix, PredicateType::Interface, contract);
			m_nondetInterfaces[contract] = createSymbolicBlock(nondetInterfaceSort(*contract, state()), "nondet_interface_" + suffix, PredicateType::NondetInterface, contract);

			for (auto const* var: stateVariablesIncludingInheritedAndPrivate(*contract))
				if (!m_context.knownVariable(*var))
					createVariable(*var);

			/// Base nondeterministic interface that allows
			/// 0 steps to be taken, used as base for the inductive
			/// rule for each function.
			auto const& iface = *m_nondetInterfaces.at(contract);
			addRule(smt::nondetInterface(iface, *contract, m_context, 0, 0), "base_nondet");

			for (auto const* base: contract->annotation().linearizedBaseContracts)
				for (auto const* function: base->definedFunctions())
				{
					for (auto var: function->parameters())
						createVariable(*var);
					for (auto var: function->returnParameters())
						createVariable(*var);
					for (auto const* var: function->localVariables())
						createVariable(*var);

					m_summaries[contract].emplace(function, createSummaryBlock(*function, *contract));

					if (
						!function->isConstructor() &&
						function->isPublic() &&
						!base->isLibrary() &&
						!base->isInterface()
					)
					{
						auto state1 = stateVariablesAtIndex(1, *contract);
						auto state2 = stateVariablesAtIndex(2, *contract);

						auto nondetPre = smt::nondetInterface(iface, *contract, m_context, 0, 1);
						auto nondetPost = smt::nondetInterface(iface, *contract, m_context, 0, 2);

						vector<smtutil::Expression> args{errorFlag().currentValue(), state().thisAddress(), state().state(1)};
						args += state1 +
							applyMap(function->parameters(), [this](auto _var) { return valueAtIndex(*_var, 0); }) +
							vector<smtutil::Expression>{state().state(2)} +
							state2 +
							applyMap(function->parameters(), [this](auto _var) { return valueAtIndex(*_var, 1); }) +
							applyMap(function->returnParameters(), [this](auto _var) { return valueAtIndex(*_var, 1); });

						connectBlocks(nondetPre, nondetPost, (*m_summaries.at(contract).at(function))(args));
					}
			}
		}
}

smtutil::Expression CHC::interface()
{
	solAssert(m_currentContract, "");
	return interface(*m_currentContract);
}

smtutil::Expression CHC::interface(ContractDefinition const& _contract)
{
	return ::interface(*m_interfaces.at(&_contract), _contract, m_context);
}

smtutil::Expression CHC::error()
{
	return (*m_errorPredicate)({});
}

smtutil::Expression CHC::error(unsigned _idx)
{
	return m_errorPredicate->functor(_idx)({});
}

smtutil::Expression CHC::summary(ContractDefinition const& _contract)
{
	return constructor(*m_constructorSummaryPredicate, _contract, m_context);
}

smtutil::Expression CHC::summary(FunctionDefinition const& _function, ContractDefinition const& _contract)
{
	return smt::function(*m_summaries.at(&_contract).at(&_function), _function, &_contract, m_context);
}

smtutil::Expression CHC::summary(FunctionDefinition const& _function)
{
	solAssert(m_currentContract, "");
	return summary(_function, *m_currentContract);
}

Predicate const* CHC::createBlock(ASTNode const* _node, PredicateType _predType, string const& _prefix)
{
	auto block = createSymbolicBlock(
		sort(_node),
		"block_" + uniquePrefix() + "_" + _prefix + predicateName(_node),
		_predType,
		_node
	);

	solAssert(m_currentFunction, "");
	return block;
}

Predicate const* CHC::createSummaryBlock(FunctionDefinition const& _function, ContractDefinition const& _contract)
{
	auto block = createSymbolicBlock(
		functionSort(_function, &_contract, state()),
		"summary_" + uniquePrefix() + "_" + predicateName(&_function, &_contract),
		PredicateType::FunctionSummary,
		&_function
	);

	return block;
}

void CHC::createErrorBlock()
{
	m_errorPredicate = createSymbolicBlock(arity0FunctionSort(), "error_target_" + to_string(m_context.newUniqueId()), PredicateType::Error);
	m_interface->registerRelation(m_errorPredicate->functor());
}

void CHC::connectBlocks(smtutil::Expression const& _from, smtutil::Expression const& _to, smtutil::Expression const& _constraints)
{
	smtutil::Expression edge = smtutil::Expression::implies(
		_from && m_context.assertions() && _constraints,
		_to
	);
	addRule(edge, _from.name + "_to_" + _to.name);
}

vector<smtutil::Expression> CHC::initialStateVariables()
{
	return stateVariablesAtIndex(0);
}

vector<smtutil::Expression> CHC::stateVariablesAtIndex(unsigned _index)
{
	solAssert(m_currentContract, "");
	return stateVariablesAtIndex(_index, *m_currentContract);
}

vector<smtutil::Expression> CHC::stateVariablesAtIndex(unsigned _index, ContractDefinition const& _contract)
{
	return applyMap(
		SMTEncoder::stateVariablesIncludingInheritedAndPrivate(_contract),
		[&](auto _var) { return valueAtIndex(*_var, _index); }
	);
}

vector<smtutil::Expression> CHC::currentStateVariables()
{
	solAssert(m_currentContract, "");
	return currentStateVariables(*m_currentContract);
}

vector<smtutil::Expression> CHC::currentStateVariables(ContractDefinition const& _contract)
{
	return applyMap(SMTEncoder::stateVariablesIncludingInheritedAndPrivate(_contract), [this](auto _var) { return currentValue(*_var); });
}

string CHC::predicateName(ASTNode const* _node, ContractDefinition const* _contract)
{
	string prefix;
	if (auto funDef = dynamic_cast<FunctionDefinition const*>(_node))
	{
		prefix += TokenTraits::toString(funDef->kind());
		if (!funDef->name().empty())
			prefix += "_" + funDef->name() + "_";
	}
	else if (m_currentFunction && !m_currentFunction->name().empty())
		prefix += m_currentFunction->name();

	auto contract = _contract ? _contract : m_currentContract;
	solAssert(contract, "");
	return prefix + "_" + to_string(_node->id()) + "_" + to_string(contract->id());
}

smtutil::Expression CHC::predicate(Predicate const& _block)
{
	switch (_block.type())
	{
	case PredicateType::Interface:
		solAssert(m_currentContract, "");
		return ::interface(_block, *m_currentContract, m_context);
	case PredicateType::ImplicitConstructor:
		solAssert(m_currentContract, "");
		return implicitConstructor(_block, *m_currentContract, m_context);
	case PredicateType::ConstructorSummary:
		solAssert(m_currentContract, "");
		return constructor(_block, *m_currentContract, m_context);
	case PredicateType::FunctionEntry:
	case PredicateType::FunctionSummary:
		solAssert(m_currentFunction, "");
		return smt::function(_block, *m_currentFunction, m_currentContract, m_context);
	case PredicateType::FunctionBlock:
		solAssert(m_currentFunction, "");
		return functionBlock(_block, *m_currentFunction, m_currentContract, m_context);
	case PredicateType::Error:
		return _block({});
	case PredicateType::NondetInterface:
		// Nondeterministic interface predicates are handled differently.
		solAssert(false, "");
	case PredicateType::Custom:
		// Custom rules are handled separately.
		solAssert(false, "");
	}
	solAssert(false, "");
}

smtutil::Expression CHC::predicate(FunctionCall const& _funCall)
{
	/// Used only for internal calls.

	auto const* function = functionCallToDefinition(_funCall);
	if (!function)
		return smtutil::Expression(true);

	errorFlag().increaseIndex();
	vector<smtutil::Expression> args{errorFlag().currentValue(), state().thisAddress(), state().state()};

	FunctionType const& funType = dynamic_cast<FunctionType const&>(*_funCall.expression().annotation().type);
	solAssert(funType.kind() == FunctionType::Kind::Internal, "");

	/// Internal calls can be made to the contract itself or a library.
	auto const* contract = function->annotation().contract;
	auto const& hierarchy = m_currentContract->annotation().linearizedBaseContracts;
	solAssert(contract->isLibrary() || find(hierarchy.begin(), hierarchy.end(), contract) != hierarchy.end(), "");

	/// If the call is to a library, we use that library as the called contract.
	/// If it is not, we use the current contract even if it is a call to a contract
	/// up in the inheritance hierarchy, since the interfaces/predicates are different.
	auto const* calledContract = contract->isLibrary() ? contract : m_currentContract;
	solAssert(calledContract, "");

	bool usesStaticCall = function->stateMutability() == StateMutability::Pure || function->stateMutability() == StateMutability::View;

	args += currentStateVariables(*calledContract);
	args += symbolicArguments(_funCall);
	if (!calledContract->isLibrary() && !usesStaticCall)
	{
		state().newState();
		for (auto const& var: m_stateVariables)
			m_context.variable(*var)->increaseIndex();
	}
	args += vector<smtutil::Expression>{state().state()};
	args += currentStateVariables(*calledContract);

	for (auto var: function->parameters() + function->returnParameters())
	{
		if (m_context.knownVariable(*var))
			m_context.variable(*var)->increaseIndex();
		else
			createVariable(*var);
		args.push_back(currentValue(*var));
	}

	return (*m_summaries.at(calledContract).at(function))(args);
}

void CHC::addRule(smtutil::Expression const& _rule, string const& _ruleName)
{
	m_interface->addRule(_rule, _ruleName);
}

pair<CheckResult, CHCSolverInterface::CexGraph> CHC::query(smtutil::Expression const& _query, langutil::SourceLocation const& _location)
{
	CheckResult result;
	CHCSolverInterface::CexGraph cex;
	tie(result, cex) = m_interface->query(_query);
	switch (result)
	{
	case CheckResult::SATISFIABLE:
	{
#ifdef HAVE_Z3
		// Even though the problem is SAT, Spacer's pre processing makes counterexamples incomplete.
		// We now disable those optimizations and check whether we can still solve the problem.
		auto* spacer = dynamic_cast<Z3CHCInterface*>(m_interface.get());
		solAssert(spacer, "");
		spacer->setSpacerOptions(false);

		CheckResult resultNoOpt;
		CHCSolverInterface::CexGraph cexNoOpt;
		tie(resultNoOpt, cexNoOpt) = m_interface->query(_query);

		if (resultNoOpt == CheckResult::SATISFIABLE)
			cex = move(cexNoOpt);

		spacer->setSpacerOptions(true);
#endif
		break;
	}
	case CheckResult::UNSATISFIABLE:
		break;
	case CheckResult::UNKNOWN:
		break;
	case CheckResult::CONFLICTING:
		m_outerErrorReporter.warning(1988_error, _location, "CHC: At least two SMT solvers provided conflicting answers. Results might not be sound.");
		break;
	case CheckResult::ERROR:
		m_outerErrorReporter.warning(1218_error, _location, "CHC: Error trying to invoke SMT solver.");
		break;
	}
	return {result, cex};
}

void CHC::addVerificationTarget(
	ASTNode const* _scope,
	VerificationTarget::Type _type,
	smtutil::Expression _from,
	smtutil::Expression _constraints,
	smtutil::Expression _errorId
)
{
	solAssert(m_currentContract || m_currentFunction, "");
	SourceUnit const* source = nullptr;
	if (m_currentContract)
		source = sourceUnitContaining(*m_currentContract);
	else
		source = sourceUnitContaining(*m_currentFunction);
	solAssert(source, "");
	if (!source->annotation().experimentalFeatures.count(ExperimentalFeature::SMTChecker))
		return;

	m_verificationTargets.emplace(_scope, CHCVerificationTarget{{_type, _from, _constraints}, _errorId});
}

void CHC::addVerificationTarget(ASTNode const* _scope, VerificationTarget::Type _type, smtutil::Expression _errorId)
{
	solAssert(m_currentContract, "");

	if (!m_currentFunction || m_currentFunction->isConstructor())
		addVerificationTarget(_scope, _type, summary(*m_currentContract), smtutil::Expression(true), _errorId);
	else
	{
		auto iface = smt::interfacePre(*m_interfaces.at(m_currentContract), *m_currentContract, m_context);
		auto sum = summary(*m_currentFunction);
		addVerificationTarget(_scope, _type, iface, sum, _errorId);
	}
}

void CHC::addAssertVerificationTarget(ASTNode const* _scope, smtutil::Expression _from, smtutil::Expression _constraints, smtutil::Expression _errorId)
{
	addVerificationTarget(_scope, VerificationTarget::Type::Assert, _from, _constraints, _errorId);
}

void CHC::checkVerificationTargets()
{
	for (auto const& [scope, target]: m_verificationTargets)
	{
		if (target.type == VerificationTarget::Type::Assert)
			checkAssertTarget(scope, target);
		else
		{
			string satMsg;
			string satMsgUnderflow;
			string satMsgOverflow;
			string unknownMsg;
			ErrorId errorReporterId;
			ErrorId underflowErrorId = 3944_error;
			ErrorId overflowErrorId = 4984_error;

			if (target.type == VerificationTarget::Type::PopEmptyArray)
			{
				solAssert(dynamic_cast<FunctionCall const*>(scope), "");
				satMsg = "Empty array \"pop\" detected here.";
				unknownMsg = "Empty array \"pop\" might happen here.";
				errorReporterId = 2529_error;
			}
			else if (
				target.type == VerificationTarget::Type::Underflow ||
				target.type == VerificationTarget::Type::Overflow ||
				target.type == VerificationTarget::Type::UnderOverflow
			)
			{
				auto const* expr = dynamic_cast<Expression const*>(scope);
				solAssert(expr, "");
				auto const* intType = dynamic_cast<IntegerType const*>(expr->annotation().type);
				if (!intType)
					intType = TypeProvider::uint256();

				satMsgUnderflow = "Underflow (resulting value less than " + formatNumberReadable(intType->minValue()) + ") happens here.";
				satMsgOverflow = "Overflow (resulting value larger than " + formatNumberReadable(intType->maxValue()) + ") happens here.";
				if (target.type == VerificationTarget::Type::Underflow)
				{
					satMsg = satMsgUnderflow;
					errorReporterId = underflowErrorId;
				}
				else if (target.type == VerificationTarget::Type::Overflow)
				{
					satMsg = satMsgOverflow;
					errorReporterId = overflowErrorId;
				}
			}
			else if (target.type == VerificationTarget::Type::DivByZero)
			{
				satMsg = "Division by zero happens here.";
				errorReporterId = 4281_error;
			}
			else
				solAssert(false, "");

			auto it = m_errorIds.find(scope->id());
			solAssert(it != m_errorIds.end(), "");
			unsigned errorId = it->second;

			if (target.type != VerificationTarget::Type::UnderOverflow)
				checkAndReportTarget(scope, target, errorId, errorReporterId, satMsg, unknownMsg);
			else
			{
				auto specificTarget = target;
				specificTarget.type = VerificationTarget::Type::Underflow;
				checkAndReportTarget(scope, specificTarget, errorId, underflowErrorId, satMsgUnderflow, unknownMsg);

				++it;
				solAssert(it != m_errorIds.end(), "");
				specificTarget.type = VerificationTarget::Type::Overflow;
				checkAndReportTarget(scope, specificTarget, it->second, overflowErrorId, satMsgOverflow, unknownMsg);
			}
		}
	}
}

void CHC::checkAssertTarget(ASTNode const* _scope, CHCVerificationTarget const& _target)
{
	solAssert(_target.type == VerificationTarget::Type::Assert, "");
	auto assertions = transactionAssertions(_scope);
	for (auto const* assertion: assertions)
	{
		auto it = m_errorIds.find(assertion->id());
		solAssert(it != m_errorIds.end(), "");
		unsigned errorId = it->second;

		checkAndReportTarget(assertion, _target, errorId, 6328_error, "Assertion violation happens here.");
	}
}

void CHC::checkAndReportTarget(
	ASTNode const* _scope,
	CHCVerificationTarget const& _target,
	unsigned _errorId,
	ErrorId _errorReporterId,
	string _satMsg,
	string _unknownMsg
)
{
	if (m_unsafeTargets.count(_scope) && m_unsafeTargets.at(_scope).count(_target.type))
		return;

	createErrorBlock();
	connectBlocks(_target.value, error(), _target.constraints && (_target.errorId == _errorId));
	auto const& [result, model] = query(error(), _scope->location());
	if (result == CheckResult::UNSATISFIABLE)
		m_safeTargets[_scope].insert(_target.type);
	else if (result == CheckResult::SATISFIABLE)
	{
		solAssert(!_satMsg.empty(), "");
		m_unsafeTargets[_scope].insert(_target.type);
		auto cex = generateCounterexample(model, error().name);
		if (cex)
			m_outerErrorReporter.warning(
				_errorReporterId,
				_scope->location(),
				"CHC: " + _satMsg,
				SecondarySourceLocation().append("\nCounterexample:\n" + *cex, SourceLocation{})
			);
		else
			m_outerErrorReporter.warning(
				_errorReporterId,
				_scope->location(),
				"CHC: " + _satMsg
			);
	}
	else if (!_unknownMsg.empty())
		m_outerErrorReporter.warning(
			_errorReporterId,
			_scope->location(),
			"CHC: " + _unknownMsg
		);
}

/**
The counterexample DAG has the following properties:
1) The root node represents the reachable error predicate.
2) The root node has 1 or 2 children:
	- One of them is the summary of the function that was called and led to that node.
	If this is the only child, this function must be the constructor.
	- If it has 2 children, the function is not the constructor and the other child is the interface node,
	that is, it represents the state of the contract before the function described above was called.
3) Interface nodes also have property 2.

The following algorithm starts collecting function summaries at the root node and repeats
for each interface node seen.
Each function summary collected represents a transaction, and the final order is reversed.

The first function summary seen contains the values for the state, input and output variables at the
error point.
*/
optional<string> CHC::generateCounterexample(CHCSolverInterface::CexGraph const& _graph, string const& _root)
{
	optional<unsigned> rootId;
	for (auto const& [id, node]: _graph.nodes)
		if (node.first == _root)
		{
			rootId = id;
			break;
		}
	if (!rootId)
		return {};

	vector<string> path;
	string localState;

	unsigned node = *rootId;
	/// The first summary node seen in this loop represents the last transaction.
	bool lastTxSeen = false;
	while (_graph.edges.at(node).size() >= 1)
	{
		auto const& edges = _graph.edges.at(node);
		solAssert(edges.size() <= 2, "");

		unsigned summaryId = edges.at(0);
		optional<unsigned> interfaceId;
		if (edges.size() == 2)
		{
			interfaceId = edges.at(1);
			if (!Predicate::predicate(_graph.nodes.at(summaryId).first)->isSummary())
				swap(summaryId, *interfaceId);
			auto interfacePredicate = Predicate::predicate(_graph.nodes.at(*interfaceId).first);
			solAssert(interfacePredicate && interfacePredicate->isInterface(), "");
		}
		/// The children are unordered, so we need to check which is the summary and
		/// which is the interface.

		Predicate const* summaryPredicate = Predicate::predicate(_graph.nodes.at(summaryId).first);
		solAssert(summaryPredicate && summaryPredicate->isSummary(), "");
		/// At this point property 2 from the function description is verified for this node.
		auto summaryArgs = _graph.nodes.at(summaryId).second;

		FunctionDefinition const* calledFun = summaryPredicate->programFunction();
		ContractDefinition const* calledContract = summaryPredicate->programContract();

		solAssert((calledFun && !calledContract) || (!calledFun && calledContract), "");
		auto stateVars = summaryPredicate->stateVariables();
		solAssert(stateVars.has_value(), "");
		auto stateValues = summaryPredicate->summaryStateValues(summaryArgs);
		solAssert(stateValues.size() == stateVars->size(), "");

		/// This summary node is the end of a tx.
		/// If it is the first summary node seen in this loop, it is the summary
		/// of the public/external function that was called when the error was reached,
		/// but not necessarily the summary of the function that contains the error.
		if (!lastTxSeen)
		{
			lastTxSeen = true;
			/// Generate counterexample message local to the failed target.
			localState = formatVariableModel(*stateVars, stateValues, ", ") + "\n";
			if (calledFun)
			{
				auto inValues = summaryPredicate->summaryPostInputValues(summaryArgs);
				auto const& inParams = calledFun->parameters();
				localState += formatVariableModel(inParams, inValues, "\n") + "\n";
				auto outValues = summaryPredicate->summaryPostOutputValues(summaryArgs);
				auto const& outParams = calledFun->returnParameters();
				localState += formatVariableModel(outParams, outValues, "\n") + "\n";
			}
		}
		else
			/// We report the state after every tx in the trace except for the last, which is reported
			/// first in the code above.
			path.emplace_back("State: " + formatVariableModel(*stateVars, stateValues, ", "));

		string txCex = summaryPredicate->formatSummaryCall(summaryArgs);
		path.emplace_back(txCex);

		/// Recurse on the next interface node which represents the previous transaction
		/// or stop.
		if (interfaceId)
		{
			Predicate const* interfacePredicate = Predicate::predicate(_graph.nodes.at(*interfaceId).first);
			solAssert(interfacePredicate && interfacePredicate->isInterface(), "");
			node = *interfaceId;
		}
		else
			break;
	}

	return localState + "\nTransaction trace:\n" + boost::algorithm::join(boost::adaptors::reverse(path), "\n");
}

string CHC::cex2dot(CHCSolverInterface::CexGraph const& _cex)
{
	string dot = "digraph {\n";

	auto pred = [&](CHCSolverInterface::CexNode const& _node) {
		return "\"" + _node.first + "(" + boost::algorithm::join(_node.second, ", ") + ")\"";
	};

	for (auto const& [u, vs]: _cex.edges)
		for (auto v: vs)
			dot += pred(_cex.nodes.at(v)) + " -> " + pred(_cex.nodes.at(u)) + "\n";

	dot += "}";
	return dot;
}

string CHC::uniquePrefix()
{
	return to_string(m_blockCounter++);
}

string CHC::contractSuffix(ContractDefinition const& _contract)
{
	return _contract.name() + "_" + to_string(_contract.id());
}

unsigned CHC::newErrorId(frontend::Expression const& _expr)
{
	unsigned errorId = m_context.newUniqueId();
	// We need to make sure the error id is not zero,
	// because error id zero actually means no error in the CHC encoding.
	if (errorId == 0)
		errorId = m_context.newUniqueId();
	m_errorIds.emplace(_expr.id(), errorId);
	return errorId;
}

SymbolicState& CHC::state()
{
	return m_context.state();
}

SymbolicIntVariable& CHC::errorFlag()
{
	return state().errorFlag();
}
