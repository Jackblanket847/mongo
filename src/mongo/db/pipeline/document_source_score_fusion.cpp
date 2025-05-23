/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/allowed_contexts.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(scoreFusion,
                                           DocumentSourceScoreFusion::LiteParsed::parse,
                                           DocumentSourceScoreFusion::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           &feature_flags::gFeatureFlagSearchHybridScoringFull);
namespace {


/**
 * Validates the weights inputs. If weights are specified by the user, there must be exactly one
 * weight per input pipeline.
 */
static void scoreFusionWeightsValidator(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& pipelines,
    const StringMap<double>& weights) {
    // The size of weights and pipelines should actually be equal, but if we have more pipelines
    // than weights, we'll throw a more specific error below that specifies which pipeline is
    // missing a weight.
    uassert(9402201,
            "$scoreFusion input has more weights than pipelines. If combination.weights is "
            "specified, there must be only one weight per named input pipeline.",
            weights.size() <= pipelines.size());

    // TODO (SERVER-100194): Change $scoreFusion weights object to accept a specified subset of
    // pipelines
    // TODO (SERVER-100511): Add typo suggestion logic for incorrectly spelled weight pipelines
    for (const auto& pipelineIt : pipelines) {
        auto pipelineName = pipelineIt.first;
        uassert(9402202,
                str::stream()
                    << "$scoreFusion input pipeline \"" << pipelineName
                    << "\" is missing a weight, even though combination.weights is specified.",
                weights.contains(pipelineName));
    }
}

/**
 * Parses and validates the weights for pipelines that have been explicitly specified in the
 * ScoreFusionSpec. Returns a map from the pipeline name to the specified weight (as a double)
 * for that pipeline.
 */
StringMap<double> extractAndValidateWeights(
    const ScoreFusionSpec& spec,
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& pipelines) {
    StringMap<double> weights;

    const auto& combinationSpec = spec.getCombination();
    if (!combinationSpec.has_value() || !combinationSpec->getWeights().has_value()) {
        return weights;
    }

    for (const auto& elem : *combinationSpec->getWeights()) {
        // elem.Number() throws a uassert if non-numeric.
        double weight = elem.Number();
        uassert(9402200,
                str::stream() << "Score fusion pipeline weight must be non-negative, but given "
                              << weight,
                weight >= 0);
        weights[elem.fieldName()] = weight;
    }
    scoreFusionWeightsValidator(pipelines, weights);
    return weights;
}

/**
 * Builds and returns an $addFields stage, like the following:
 * {$addFields:
 *     {prefix_score:
 *         {$multiply:
 *             [{"$score"}, 0.5] // or [{$meta: "vectorSearchScore"}, 0.5]
 *         },
 *     }
 * }
 */
boost::intrusive_ptr<DocumentSource> buildScoreAddFieldsStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const StringData inputPipelineName,
    const BSONObj& scorePath,
    const ScoreFusionNormalizationEnum normalization,
    const double weight) {
    const std::string score = fmt::format("{}_score", inputPipelineName);
    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        {
            BSONObjBuilder scoreField(addFieldsBob.subobjStart(score));
            {
                BSONArrayBuilder multiplyArray(scoreField.subarrayStart("$multiply"_sd));
                BSONObj normalizationScorePath;
                switch (normalization) {
                    case ScoreFusionNormalizationEnum::kSigmoid:
                        normalizationScorePath = BSON("$sigmoid" << scorePath);
                        break;
                    case ScoreFusionNormalizationEnum::kMinMaxScaler:
                        // TODO SERVER-100211: Handle minMaxScaler expression behavior.
                        uasserted(ErrorCodes::NotImplemented,
                                  "minMaxScaler input normalization is not yet supported");
                        break;
                    case ScoreFusionNormalizationEnum::kNone:
                        // In the case of no normalization, parse just the score operator
                        // itself.
                        normalizationScorePath = std::move(scorePath);
                        break;
                }
                multiplyArray.append(normalizationScorePath);
                multiplyArray.append(weight);
            }
        }
    }

    const BSONObj spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Builds and returns a $replaceRoot stage: {$replaceWith: {docs: "$$ROOT"}}.
 * This has the effect of storing the unmodified user's document in the path '$docs'.
 */
boost::intrusive_ptr<DocumentSource> buildReplaceRootStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return DocumentSourceReplaceRoot::createFromBson(
        BSON("$replaceWith" << BSON("docs" << "$$ROOT")).firstElement(), expCtx);
}

/**
 * Build stages for first pipeline. Example where the first pipeline is called "name1" and has a
 * weight of 5.0:
 * { ... stages of first pipeline ... }
 * { "$replaceRoot": { "newRoot": { "docs": "$$ROOT" } } },
 * { "$addFields": { "name1_score": { "$multiply": [ { $meta: "score" }, { "$const": 5.0 } ] } } }
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildFirstPipelineStages(
    const StringData inputPipelineOneName,
    const BSONObj& scorePath,
    const ScoreFusionNormalizationEnum normalization,
    const double weight,
    const std::unique_ptr<Pipeline, PipelineDeleter>& firstInputPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;

    while (!firstInputPipeline->getSources().empty()) {
        // These stages are being copied over from the original pipeline.
        outputStages.push_back(firstInputPipeline->popFront());
    }

    outputStages.emplace_back(buildReplaceRootStage(expCtx));
    outputStages.emplace_back(
        buildScoreAddFieldsStage(expCtx, inputPipelineOneName, scorePath, normalization, weight));
    return outputStages;
}

/**
 * Checks that the input pipeline is a valid scored pipeline. This means it is either one of
 * $search, $vectorSearch, $scoreFusion, $rankFusion (which have scored output) or has an explicit
 * $score stage. A scored pipeline must also be a 'selection pipeline', which means no stage can
 * modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 */
static void scoreFusionPipelineValidator(const Pipeline& pipeline) {
    // Note that we don't check for $rankFusion and $scoreFusion explicitly because it will be
    // desugared by this point.
    static const std::set<StringData> implicitlyScoredStages{DocumentSourceVectorSearch::kStageName,
                                                             DocumentSourceSearch::kStageName};
    auto sources = pipeline.getSources();

    static const std::string scorePipelineMsg =
        "All subpipelines to the $scoreFusion stage must begin with one of $search, "
        "$vectorSearch, $rankFusion, $scoreFusion or have a custom $score in the pipeline.";
    uassert(9402503,
            str::stream() << "$scoreFusion input pipeline cannot be empty. " << scorePipelineMsg,
            !sources.empty());

    uassert(9402500, scorePipelineMsg, hybrid_scoring_util::isScoredPipeline(pipeline));

    std::for_each(sources.begin(), sources.end(), [](auto& stage) {
        if (stage->getSourceName() == DocumentSourceSearch::kStageName) {
            uassert(
                9402501,
                str::stream()
                    << "$search can be used in a $scoreFusion subpipeline but not when "
                       "returnStoredSource is set to true because it modifies the output fields. "
                       "Only stages that retrieve, limit, or order documents are allowed.",
                stage->constraints().noFieldModifications);
        } else {
            uassert(9402502,
                    str::stream() << stage->getSourceName()
                                  << " is not allowed in a $scoreFusion subpipeline because it "
                                     "modifies the documents or transforms their fields. Only "
                                     "stages that retrieve, limit, or order documents are allowed.",
                    stage->constraints().noFieldModifications);
        }
    });
}

/**
 * Group all the input documents across all pipelines and their respective score fields. Turn null
 * scores into 0.
 * { "$group": { "_id": "$docs._id", "docs": { "$first": "$docs" },
 * "name1_score": { "$max": {"$ifNull": [ "$name1_score", 0 ] } } } }
 */
BSONObj groupEachScore(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& pipelines) {
    // For each sub-pipeline, build the following obj:
    // name_score: {$max: {ifNull: ["$name_score", 0]}}
    BSONObjBuilder bob;
    {
        BSONObjBuilder groupBob(bob.subobjStart("$group"_sd));
        groupBob.append("_id", "$docs._id");
        groupBob.append("docs", BSON("$first" << "$docs"));

        for (auto pipeline_it = pipelines.begin(); pipeline_it != pipelines.end(); pipeline_it++) {
            const auto& pipelineName = pipeline_it->first;
            const std::string scoreName = fmt::format("{}_score", pipelineName);
            groupBob.append(
                scoreName,
                BSON("$max" << BSON("$ifNull" << BSON_ARRAY(fmt::format("${}", scoreName) << 0))));
        }
        groupBob.done();
    }
    bob.done();
    return bob.obj();
}

/**
 * Calculate the final score by summing the score fields on each input document and adding it as a
 * new field to the document.
 * { "$setMetadata": { "score": { "$add": ["$name1_score", "$name2_score"] } } },
 */
boost::intrusive_ptr<DocumentSource> buildSetScoreStage(
    const auto& expCtx,
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputPipelines,
    const DocumentSourceScoreFusion::ScoreCombination scoreFusionCombination) {
    ScoreFusionCombinationMethodEnum combinationMethod =
        scoreFusionCombination.getCombinationMethod();
    // Default is to sum the scores.
    boost::intrusive_ptr<Expression> metadataExpression;
    switch (combinationMethod) {
        case ScoreFusionCombinationMethodEnum::kExpression: {
            boost::optional<IDLAnyType> combinationExpression =
                scoreFusionCombination.getCombinationExpression();
            // Earlier logic checked that combination.expression's value must be present if
            // combination.method has the value 'expression.'

            // Assemble $let.vars field. It is a BSON obj of pipeline names to their corresponding
            // pipeline score field. Ex: {geo_doc: "$geo_doc_score"}.
            BSONObjBuilder varsAndInFields;
            for (auto pipeline_it = inputPipelines.begin(); pipeline_it != inputPipelines.end();
                 pipeline_it++) {
                std::string fieldScoreName = fmt::format("${}_score", pipeline_it->first);
                varsAndInFields.appendElements(BSON(pipeline_it->first << fieldScoreName));
            }
            varsAndInFields.done();

            // Assemble $let expression. For example: { "$let": { "vars": { "geo_doc":
            // "$geo_doc_score" }, "in": { "$sum": ["$$geo_doc", 5.0] } } },
            // where the user-inputted combination.expression is: { "$sum": ["$$geo_doc", 5.0] }
            // This is done so the user-inputted pipeline name variables correctly evaluate to each
            // pipeline's underlying score field path. Ex: pipeline name $$geo_doc maps to
            // $geo_doc_score.

            // At this point, we can't be sure that the user-provided expression evaluates to a
            // numeric type. However, upon attempting to set the metadata score field with this
            // expression, if it does not evaluate to a numeric type, then we will throw a
            // TypeMismatch error.
            metadataExpression = ExpressionLet::parse(
                expCtx.get(),
                BSON("$let" << BSON("vars" << varsAndInFields.obj() << "in"
                                           << combinationExpression->getElement()))
                    .firstElement(),
                expCtx->variablesParseState);
            break;
        }
        case ScoreFusionCombinationMethodEnum::kAvg: {
            // Construct an array of the score field path names for AccumulatorAvg.
            BSONArrayBuilder expressionFieldPaths;
            for (auto pipeline_it = inputPipelines.begin(); pipeline_it != inputPipelines.end();
                 pipeline_it++) {
                std::string fieldScoreName = fmt::format("${}_score", pipeline_it->first);
                expressionFieldPaths.append(fieldScoreName);
            }
            expressionFieldPaths.done();
            metadataExpression = ExpressionFromAccumulator<AccumulatorAvg>::parse(
                expCtx.get(),
                BSON("$avg" << expressionFieldPaths.arr()).firstElement(),
                expCtx->variablesParseState);
            break;
        }
        case ScoreFusionCombinationMethodEnum::kSum: {
            Expression::ExpressionVector allInputScores;
            for (auto pipeline_it = inputPipelines.begin(); pipeline_it != inputPipelines.end();
                 pipeline_it++) {
                std::string fieldScoreName = fmt::format("{}_score", pipeline_it->first);
                allInputScores.push_back(ExpressionFieldPath::createPathFromString(
                    expCtx.get(), fieldScoreName, expCtx->variablesParseState));
            }

            metadataExpression =
                make_intrusive<ExpressionAdd>(expCtx.get(), std::move(allInputScores));
            break;
        }
        default:
            // Only one of the above options can be specified for combination.method.
            MONGO_UNREACHABLE_TASSERT(10016700);
    }
    return DocumentSourceSetMetadata::create(
        expCtx, metadataExpression, DocumentMetadataFields::MetaType::kScore);
}

/**
 * Build the pipeline input to $unionWith (consists of a $replaceRoot and $addFields stage). Returns
 * a $unionWith stage that looks something like this:
 * { "$unionWith": { "coll": "pipeline_test", "pipeline": [inputPipeline stage(ex: $vectorSearch),
 * $replaceRoot stage, $addFields stage] } }
 */
boost::intrusive_ptr<DocumentSource> buildUnionWithPipelineStage(
    const std::string& inputPipelineName,
    const BSONObj& scorePath,
    const ScoreFusionNormalizationEnum normalization,
    const double weight,
    const std::unique_ptr<Pipeline, PipelineDeleter>& oneInputPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // Here, a copy of the original input pipeline is created,
    // as additional stages need to be appended to it before it can be wrapped in the $unionWith.
    std::unique_ptr<Pipeline, PipelineDeleter> inputPipelineStages = oneInputPipeline->clone();

    inputPipelineStages->pushBack(buildReplaceRootStage(expCtx));
    inputPipelineStages->pushBack(
        buildScoreAddFieldsStage(expCtx, inputPipelineName, scorePath, normalization, weight));
    std::vector<BSONObj> bsonPipeline = inputPipelineStages->serializeToBson();

    auto collName = expCtx->getNamespaceString().coll();

    BSONObj inputToUnionWith =
        BSON("$unionWith" << BSON("coll" << collName << "pipeline" << bsonPipeline));
    return DocumentSourceUnionWith::createFromBson(inputToUnionWith.firstElement(), expCtx);
}

/**
 * After all the pipelines have been executed and unioned, builds the $group stage to merge the
 * scoreFields/apply score nulls behavior, calculate the final score field to add to each document,
 * sorts the documents by score and id, and replaces the root with the final set of outputted
 * documents.
 * The $sort stage looks like this: { "$sort": { "score": {$meta: "score"}, "_id": 1 } }
 * The $replaceRoot stage looks like this: { "$replaceRoot": { "newRoot": "$docs" } }
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildScoreAndMergeStages(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputPipelines,
    const DocumentSourceScoreFusion::ScoreCombination combination,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto group =
        DocumentSourceGroup::createFromBson(groupEachScore(inputPipelines).firstElement(), expCtx);
    auto setScoreMeta = buildSetScoreStage(expCtx, inputPipelines, combination);

    const SortPattern sortingPattern{BSON("score" << BSON("$meta" << "score") << "_id" << 1),
                                     expCtx};
    auto sort = DocumentSourceSort::create(expCtx, sortingPattern);

    auto restoreUserDocs =
        DocumentSourceReplaceRoot::create(expCtx,
                                          ExpressionFieldPath::createPathFromString(
                                              expCtx.get(), "docs", expCtx->variablesParseState),
                                          "documents",
                                          SbeCompatibility::noRequirements);
    return {std::move(group), std::move(setScoreMeta), std::move(sort), std::move(restoreUserDocs)};
}
}  // namespace

std::unique_ptr<DocumentSourceScoreFusion::LiteParsed> DocumentSourceScoreFusion::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName << " must take a nested object but found: " << spec,
            spec.type() == BSONType::Object);

    auto parsedSpec = ScoreFusionSpec::parse(IDLParserContext(kStageName), spec.embeddedObject());
    auto inputPipesObj = parsedSpec.getInput().getPipelines();

    // Parse each pipeline.
    std::vector<LiteParsedPipeline> liteParsedPipelines;
    for (const auto& elem : inputPipesObj) {
        liteParsedPipelines.emplace_back(nss, parsePipelineFromBSON(elem));
    }

    return std::make_unique<DocumentSourceScoreFusion::LiteParsed>(
        spec.fieldName(), nss, std::move(liteParsedPipelines));
}

/**
 * Validate that each pipeline is a valid scored selection pipeline. Returns a pair of the map of
 * the input pipeline names to pipeline objects and a map of pipeline names to score paths.
 */
std::pair<std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>,
          std::map<std::string, BSONObj>>
parseAndValidateScoredSelectionPipelines(const BSONElement& elem,
                                         const ScoreFusionSpec& spec,
                                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>> inputPipelines;
    std::map<std::string, BSONObj> scorePaths;
    for (const auto& elem : spec.getInput().getPipelines()) {
        auto pipeline = Pipeline::parse(parsePipelineFromBSON(elem), pExpCtx);
        // Ensure that all pipelines are valid scored selection pipelines.
        scoreFusionPipelineValidator(*pipeline);

        // Validate pipeline name.
        auto inputName = elem.fieldName();
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(inputName),
            "$scoreFusion pipeline names must follow the naming rules of field path expressions.");
        uassert(9402203,
                str::stream()
                    << "$scoreFusion pipeline names must be unique, but found duplicate name '"
                    << inputName << "'.",
                !inputPipelines.contains(inputName));
        BSONObj scorePath = BSON("$meta" << "score");

        // Input pipeline has been validated; save it in the resulting maps.
        scorePaths[inputName] = BSON("$meta" << "score");
        inputPipelines[inputName] = std::move(pipeline);
    }
    return {std::move(inputPipelines), std::move(scorePaths)};
}

// To fully understand the structure of the desugared output returned from this function, you
// can read the desugared output in the CheckOnePipelineAllowed and
// CheckMultiplePipelinesAllowed test cases under document_source_score_fusion_test.cpp.
std::list<boost::intrusive_ptr<DocumentSource>> constructDesugaredOutput(
    const ScoreFusionSpec& spec,
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputPipelines,
    const std::map<std::string, BSONObj>& scorePaths,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    StringMap<double> weights = extractAndValidateWeights(spec, inputPipelines);
    ScoreFusionNormalizationEnum normalization = spec.getInput().getNormalization();
    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;
    for (auto pipeline_it = inputPipelines.begin(); pipeline_it != inputPipelines.end();
         pipeline_it++) {
        const auto& [inputPipelineName, inputPipelineStages] = *pipeline_it;

        // Check if an explicit weight for this pipeline has been specified.
        // If not, the default is one.
        double pipelineWeight = weights.empty() ? 1 : weights.at(inputPipelineName);

        if (pipeline_it == inputPipelines.begin()) {
            // Stages for the first pipeline.
            auto firstPipelineStages = buildFirstPipelineStages(inputPipelineName,
                                                                scorePaths.at(inputPipelineName),
                                                                normalization,
                                                                pipelineWeight,
                                                                inputPipelineStages,
                                                                pExpCtx);
            for (auto&& stage : firstPipelineStages) {
                outputStages.emplace_back(stage);
            }
        } else {
            // For the input pipelines other than the first,
            // we wrap then in a $unionWith stage to append it to the total desugared output.
            auto unionWithStage = buildUnionWithPipelineStage(inputPipelineName,
                                                              scorePaths.at(inputPipelineName),
                                                              normalization,
                                                              pipelineWeight,
                                                              inputPipelineStages,
                                                              pExpCtx);
            outputStages.emplace_back(unionWithStage);
        }
    }
    // Build all remaining stages to perform the fusion.
    // The ScoreCombination class sets the combination.method and combination.expression to the
    // correct user input after performing the necessary error checks (ex: verify that if
    // combination.method is expression, then the combination.expression should've been specified).
    // Sum is the default combination method if no other method is specified.
    DocumentSourceScoreFusion::ScoreCombination scoreFusionCombination(spec);
    auto finalStages = buildScoreAndMergeStages(inputPipelines, scoreFusionCombination, pExpCtx);
    for (auto&& stage : finalStages) {
        outputStages.emplace_back(stage);
    }
    return outputStages;
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceScoreFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);
    auto spec = ScoreFusionSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());

    const auto& [inputPipelines, scorePaths] =
        parseAndValidateScoredSelectionPipelines(elem, spec, pExpCtx);
    return constructDesugaredOutput(spec, inputPipelines, scorePaths, pExpCtx);
}
}  // namespace mongo
