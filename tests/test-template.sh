#!/bin/sh
#
# Template for a new test with a name of the form "123_Test_description.test".
#
# Copyright 2026 Andrew Wood
# License GPLv3+: GNU GPL version 3 or later; see `docs/COPYING'.

error="not set - call this from 'make check'"
true "${sourcePath:?$error}"
true "${workDir:?$error}"
test -n "${sourcePath}" && . "${sourcePath}/tests/test-functions.sh"

exitStatus=0

# If there are multiple sub-tests, start each one with "startSubTest".
# To nudge the automatic calculation of the total number of sub-tests to add
# a few more, such as when calling sub-tests inside a loop of fixed number
# of iterations, add a comment "# subTestCount:+1" where 1 is how many to
# add.  To override automatic calculation entirely, set the "subTestCount"
# variable to the total before calling "startSubTest".

# Run the test subject with "runTestSubject" instead of "./pv"; this allows
# the generation of an accompanying memory-safety version of this test, in
# which "runTestSubject" runs valgrind.  Where this wouldn't be appropriate,
# run the test subject with "${testSubject}" instead (again, instead of
# "./pv").

# Use "reportFailure" to report a failure - this sets exitStatus to 1 as
# well as reporting the message; but if it's running in a subshell (such as
# when in a pipeline), exitStatus won't propagate out to the parent, so the
# parent needs to set it explicitly.

# Write temporary files under "${workDir}".

# runTestSubject ... || reportFailure "describe this specific failure"

exit ${exitStatus}
