#!/bin/bash

wget $(curl -s https://ci.swift.org/job/oss-swift-package-centos-7/lastSuccessfulBuild/consoleText | grep 'tmp-ci-nightly' | sed 's/-- /\n/g' | tail -1 | sed 's#https://store-030.blobstore.apple.com/swift-oss#https://download.swift.org#g')