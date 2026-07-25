file(READ "${GRAMMAR}" grammar)
string(REPLACE "%define api.pure\n" "" grammar "${grammar}")
file(WRITE "${GRAMMAR}" "${grammar}")
