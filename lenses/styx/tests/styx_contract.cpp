#include "tests/support/lens_contract.hpp"
TEST_CASE("Styx built-in Lens contract") { tokmon::tests::verify_lens_contract("styx", false); }
