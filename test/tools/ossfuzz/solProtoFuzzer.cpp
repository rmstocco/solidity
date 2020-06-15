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

#include <test/tools/ossfuzz/protoToSol.h>
#include <test/tools/ossfuzz/solProto.pb.h>
#include <test/tools/ossfuzz/abiV2FuzzerCommon.h>
#include <test/EVMHost.h>

#include <evmone/evmone.h>
#include <src/libfuzzer/libfuzzer_macro.h>

#include <fstream>

static evmc::VM evmone = evmc::VM{evmc_create_evmone()};

using namespace solidity::test::abiv2fuzzer;
using namespace solidity::test::solprotofuzzer;
using namespace solidity;
using namespace solidity::test;
using namespace solidity::util;
using namespace std;

namespace
{
/// Test function returns a uint256 value
static size_t const expectedOutputLength = 32;
/// Expected output value is decimal 0
static uint8_t const expectedOutput[expectedOutputLength] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/// Compares the contents of the memory address pointed to
/// by `_result` of `_length` bytes to the expected output.
/// Returns true if `_result` matches expected output, false
/// otherwise.
bool isOutputExpected(evmc::result const& _run)
{
	if (_run.output_size != expectedOutputLength)
		return false;

	return memcmp(_run.output_data, expectedOutput, expectedOutputLength) == 0;
}

/// Accepts a reference to a user-specified input and returns an
/// evmc_message with all of its fields zero initialized except
/// gas and input fields.
/// The gas field is set to the maximum permissible value so that we
/// don't run into out of gas errors. The input field is copied from
/// user input.
evmc_message initializeMessage(bytes const& _input)
{
	// Zero initialize all message fields
	evmc_message msg = {};
	// Gas available (value of type int64_t) is set to its maximum
	// value.
	msg.gas = std::numeric_limits<int64_t>::max();
	msg.input_data = _input.data();
	msg.input_size = _input.size();
	return msg;
}

/// Accepts host context implementation, and keccak256 hash of the function
/// to be called at a specified address in the simulated blockchain as
/// input and returns the result of the execution of the called function.
evmc::result executeContract(
	EVMHost& _hostContext,
	bytes const& _functionHash,
	evmc_address _deployedAddress
)
{
	evmc_message message = initializeMessage(_functionHash);
	message.destination = _deployedAddress;
	message.kind = EVMC_CALL;
	return _hostContext.call(message);
}

/// Accepts a reference to host context implementation and byte code
/// as input and deploys it on the simulated blockchain. Returns the
/// result of deployment.
evmc::result deployContract(EVMHost& _hostContext, bytes const& _code)
{
	evmc_message message = initializeMessage(_code);
	message.kind = EVMC_CREATE;
	return _hostContext.call(message);
}

std::pair<bytes, Json::Value> compileContract(
	std::string _sourceCode,
	std::string _contractName,
	std::map<std::string, solidity::util::h160> const& _libraryAddresses = {},
	frontend::OptimiserSettings _optimization = frontend::OptimiserSettings::minimal()
)
{
	try
	{
		// Compile contract generated by the proto fuzzer
		SolidityCompilationFramework solCompilationFramework;
		return std::make_pair(
			solCompilationFramework.compileContract(_sourceCode, _contractName, _libraryAddresses, _optimization),
			solCompilationFramework.getMethodIdentifiers()
		);
	}
	// Ignore stack too deep errors during compilation
	catch (evmasm::StackTooDeepException const&)
	{
		return std::make_pair(bytes{}, Json::Value(0));
	}
}

evmc::result deployAndExecute(EVMHost& _hostContext, bytes _byteCode, std::string _hexEncodedInput)
{
	// Deploy contract and signal failure if deploy failed
	evmc::result createResult = deployContract(_hostContext, _byteCode);
	solAssert(
		createResult.status_code == EVMC_SUCCESS,
		"Proto solc fuzzer: Contract creation failed"
	);

	// Execute test function and signal failure if EVM reverted or
	// did not return expected output on successful execution.
	evmc::result callResult = executeContract(
		_hostContext,
		fromHex(_hexEncodedInput),
		createResult.create_address
	);

	// We don't care about EVM One failures other than EVMC_REVERT
	solAssert(callResult.status_code != EVMC_REVERT, "Proto solc fuzzer: EVM One reverted");
	return callResult;
}

evmc::result compileDeployAndExecute(
	std::string _sourceCode,
	std::string _contractName,
	std::string _methodName,
	frontend::OptimiserSettings _optimization,
	std::string _libraryName = {}
)
{
	bytes libraryBytecode;
	Json::Value libIds;
	// We target the default EVM which is the latest
	langutil::EVMVersion version = {};
	EVMHost hostContext(version, evmone);
	std::map<std::string, solidity::util::h160> _libraryAddressMap;

	// First deploy library
	if (!_libraryName.empty())
	{
		tie(libraryBytecode, libIds) = compileContract(
			_sourceCode,
			_libraryName,
			{},
			_optimization
		);
		// Deploy contract and signal failure if deploy failed
		evmc::result createResult = deployContract(hostContext, libraryBytecode);
		solAssert(
			createResult.status_code == EVMC_SUCCESS,
			"Proto solc fuzzer: Library deployment failed"
		);
		_libraryAddressMap[_libraryName] = EVMHost::convertFromEVMC(createResult.create_address);
	}

	auto [bytecode, ids] = compileContract(
		_sourceCode,
		_contractName,
		_libraryAddressMap,
		_optimization
	);

	return deployAndExecute(
		hostContext,
		bytecode,
		ids[_methodName].asString()
	);
}
}

DEFINE_PROTO_FUZZER(Program const& _input)
{
	ProtoConverter converter;
	string sol_source = converter.protoToSolidity(_input);

	if (char const* dump_path = getenv("PROTO_FUZZER_DUMP_PATH"))
	{
		// With libFuzzer binary run this to generate a YUL source file x.yul:
		// PROTO_FUZZER_DUMP_PATH=x.yul ./a.out proto-input
		ofstream of(dump_path);
		of.write(sol_source.data(), static_cast<unsigned>(sol_source.size()));
	}

	if (char const* dump_path = getenv("SOL_DEBUG_FILE"))
	{
		sol_source.clear();
		// With libFuzzer binary run this to generate a YUL source file x.yul:
		// PROTO_FUZZER_LOAD_PATH=x.yul ./a.out proto-input
		ifstream ifstr(dump_path);
		sol_source = {
			std::istreambuf_iterator<char>(ifstr),
			std::istreambuf_iterator<char>()
		};
		std::cout << sol_source << std::endl;
	}

	auto minimalResult = compileDeployAndExecute(
		sol_source,
		":C",
		"test()",
		frontend::OptimiserSettings::minimal(),
		converter.libraryTest() ? converter.libraryName() : ""
	);
	bool successState = minimalResult.status_code == EVMC_SUCCESS;
	if (successState)
		solAssert(
			isOutputExpected(minimalResult),
			"Proto solc fuzzer: Output incorrect"
		);
}
