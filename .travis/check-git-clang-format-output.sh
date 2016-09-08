#!/bin/bash
output="$(.travis/git-clang-format --binary clang-format-3.8 --commit $TRAVIS_BRANCH --diff)"
if [ "$output" == "no modified files to format" ] || [ "$output" == "clang-format did not modify any files" ] ; then
  echo "clang-format passed."
  exit 0
else
  echo "clang-format failed:"
  echo "$output"
  exit 1
fi
