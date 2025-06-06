/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(LimitThenSkip, SkipIsCappedWhenLargerThanLimit) {
    LimitThenSkip source(5, 10);
    ASSERT_EQ(*source.getLimit(), 5);
    ASSERT_EQ(*source.getSkip(), 5);
}

TEST(LimitThenSkip, FlipToSkipThenLimit) {
    LimitThenSkip source(15, 5);
    SkipThenLimit converted = source.flip();
    ASSERT_EQ(*converted.getSkip(), 5);
    ASSERT_EQ(*converted.getLimit(), 10);
}

TEST(LimitThenSkip, FlipOnlyLimit) {
    LimitThenSkip source(15, boost::none);
    SkipThenLimit converted = source.flip();
    ASSERT(!converted.getSkip());
    ASSERT_EQ(*converted.getLimit(), 15);
}

TEST(LimitThenSkip, FlipOnlySkip) {
    LimitThenSkip source(boost::none, 5);
    SkipThenLimit converted = source.flip();
    ASSERT_EQ(*converted.getSkip(), 5);
    ASSERT(!converted.getLimit());
}

}  // namespace
}  // namespace mongo
