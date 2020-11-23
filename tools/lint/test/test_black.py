# -*- coding: utf-8 -*-

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import mozunit

LINTER = "black"


def test_lint_black(lint, paths):
    results = lint(paths())
    assert len(results) == 2

    print(results)
    assert results[0].level == "error"
    assert results[0].relpath == "bad.py"

    assert "EOF" in results[1].message
    assert results[1].level == "error"
    assert results[1].relpath == "invalid.py"


if __name__ == "__main__":
    mozunit.main()
