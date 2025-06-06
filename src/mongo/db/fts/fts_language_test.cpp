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

#include "mongo/db/fts/fts_language.h"

#include <ostream>

#include "mongo/base/error_codes.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace fts {

namespace {

using LanguageMakeException = mongo::ExceptionFor<ErrorCodes::BadValue>;

TEST(FTSLanguageV3, Make) {
    static constexpr auto kVer = TEXT_INDEX_VERSION_3;
    ASSERT_EQUALS(FTSLanguage::make("spanish", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("es", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("SPANISH", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("ES", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("none", kVer).str(), "none");
    ASSERT_THROWS(FTSLanguage::make("", kVer), LanguageMakeException);
    ASSERT_THROWS(FTSLanguage::make("spanglish", kVer), LanguageMakeException);
}

TEST(FTSLanguageV2, Make) {
    static constexpr auto kVer = TEXT_INDEX_VERSION_2;
    ASSERT_EQUALS(FTSLanguage::make("spanish", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("es", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("SPANISH", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("ES", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("none", kVer).str(), "none");
    ASSERT_THROWS(FTSLanguage::make("spanglish", kVer), LanguageMakeException);
    ASSERT_THROWS(FTSLanguage::make("", kVer), LanguageMakeException);
}

TEST(FTSLanguageV1, Make) {
    static constexpr auto kVer = TEXT_INDEX_VERSION_1;
    ASSERT_EQUALS(FTSLanguage::make("spanish", kVer).str(), "spanish");
    ASSERT_EQUALS(FTSLanguage::make("porter", kVer).str(), "porter") << "deprecated";
    ASSERT_EQUALS(FTSLanguage::make("en", kVer).str(), "en");
    ASSERT_EQUALS(FTSLanguage::make("eng", kVer).str(), "eng");
    ASSERT_EQUALS(FTSLanguage::make("none", kVer).str(), "none");
    // Negative V1 tests
    ASSERT_EQUALS(FTSLanguage::make("SPANISH", kVer).str(), "none") << "case sensitive";
    ASSERT_EQUALS(FTSLanguage::make("asdf", kVer).str(), "none") << "unknown";
    ASSERT_EQUALS(FTSLanguage::make("", kVer).str(), "none");
}

}  // namespace
}  // namespace fts
}  // namespace mongo
