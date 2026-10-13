#include "Processor.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>

// ####################
// CHAIN MANAGEMENT
// ####################

// The lane loadTone inserts into: Left in mono mode, or the side the UI armed
// before launching the Select flow in stereo.
std::vector<std::unique_ptr<ChainBlock>>& TONE3000Processor::activeChain() {
  if (stereoEnabled.load() && pendingAddSide == ChainSide::Right)
    return lane(ChainSide::Right);
  return lane(ChainSide::Left);
}

// Find a block by id across both lanes (ids are globally unique).
ChainBlock* TONE3000Processor::findBlockById(const std::string& blockId) {
  for (auto& l : lanes)
    for (auto& b : l)
      if (b && b->id == blockId)
        return b.get();
  return nullptr;
}

namespace {

bool isInsertBlock(const std::unique_ptr<ChainBlock>& b) {
  return b != nullptr && b->type == ChainBlockType::INSERT;
}

}  // namespace

// See the declaration for the invariant. Called after every structural lane
// change (load/remove/cross-lane move/stereo seed/snapshot restore); pure
// bookkeeping: no revision bump, no history entry of its own.
void TONE3000Processor::normalizeLaneInserts(Lane& l) {
  const int total = static_cast<int>(l.size());
  int inserts = static_cast<int>(std::count_if(l.begin(), l.end(), isInsertBlock));
  const int tones = total - inserts;
  const int required = std::max(kMinLaneSlots - tones, 1);

  // Trim overshoot back-to-front so slots the user positioned stay put.
  for (int i = total - 1; i >= 0 && inserts > required; --i) {
    if (isInsertBlock(l[static_cast<size_t>(i)])) {
      l.erase(l.begin() + i);
      --inserts;
    }
  }

  while (inserts < required) {
    l.push_back(std::make_unique<ChainBlock>(juce::Uuid().toString().toStdString(),
                                             ChainBlockType::INSERT));
    ++inserts;
  }
}

namespace {

// Everything loadTone/swapTone need from a raw tone JSON string.
struct ParsedTone {
  bool valid = false;
  int toneId = 0;
  int firstModelId = 0;
  juce::String modelUrl;
  juce::String modelName;
  ChainBlockType type = ChainBlockType::NAM;
  // Parsed tone with its models pruned to just the one being loaded; native
  // only ever stores the active model; the catalog stays on the API and the
  // UI pages it in for the picker.
  juce::var toneVar;
  juce::String toneJson;  // `toneVar` re-serialized (what the block persists)
};

// The engine type a tone requires, from its parsed JSON (`format`, with the
// legacy `platform` fallback). While a tone swap is in flight the block's own
// `type` still describes the *old* engine (which keeps processing until the
// new model is applied), so loads must key off the tone, not the block.
ChainBlockType toneEngineType(const juce::var& toneVar, ChainBlockType fallback) {
  auto* obj = toneVar.getDynamicObject();
  if (obj == nullptr)
    return fallback;
  juce::String format = obj->getProperty("format").toString().toLowerCase();
  if (format.isEmpty())
    format = obj->getProperty("platform").toString().toLowerCase();
  if (format.isEmpty())
    return fallback;
  return format == "nam" ? ChainBlockType::NAM : ChainBlockType::IR;
}

ParsedTone parseToneForLoading(const juce::String& toneJsonString) {
  ParsedTone out;

  juce::var toneVar = juce::JSON::parse(toneJsonString);
  juce::DynamicObject* toneObj = toneVar.getDynamicObject();
  if (toneObj == nullptr) {
    DBG("Tone JSON is not a valid object");
    return out;
  }

  out.toneId = toneObj->getProperty("id");
  // The API renamed `platform` to `format`; fall back to `platform` for tone
  // JSON persisted by older builds.
  juce::String format = toneObj->getProperty("format").toString().toLowerCase();
  if (format.isEmpty())
    format = toneObj->getProperty("platform").toString().toLowerCase();
  juce::var modelsVar = toneObj->getProperty("models");

  if (!modelsVar.isArray() || modelsVar.getArray()->size() == 0) {
    DBG("Tone has no models");
    return out;
  }

  juce::DynamicObject* firstModel = modelsVar.getArray()->getReference(0).getDynamicObject();
  if (firstModel == nullptr) {
    DBG("First model is not a valid object");
    return out;
  }

  out.firstModelId = firstModel->getProperty("id");
  out.modelUrl = firstModel->getProperty("model_url").toString();
  out.modelName = firstModel->getProperty("name").toString();
  out.type = (format == "nam") ? ChainBlockType::NAM : ChainBlockType::IR;

  // Store only the model being loaded; native persists just the active
  // model; the catalog stays on the API. Local tones are the exception:
  // their model list *is* the dropped files (no API to page the others back
  // in from), so it stays whole.
  if (!static_cast<bool>(toneObj->getProperty("local"))) {
    juce::Array<juce::var> prunedModels;
    prunedModels.add(modelsVar.getArray()->getReference(0));
    toneObj->setProperty("models", prunedModels);
  }

  out.toneVar = toneVar;
  out.toneJson = juce::JSON::toString(toneVar);
  out.valid = true;
  return out;
}

}  // namespace

juce::var TONE3000Processor::makeToneSummary(const juce::var& toneVar) {
  auto* tone = toneVar.getDynamicObject();
  if (tone == nullptr)
    return {};

  juce::DynamicObject::Ptr out = new juce::DynamicObject();
  out->setProperty("id", tone->getProperty("id"));
  out->setProperty("title", tone->getProperty("title"));
  // Older persisted tone JSON used `platform` instead of `format`.
  juce::var format = tone->getProperty("format");
  if (format.toString().isEmpty())
    format = tone->getProperty("platform");
  out->setProperty("format", format);
  out->setProperty("gear", tone->getProperty("gear"));

  // Drop-loaded local file(s) (see loadLocalTone): no catalog metadata
  // exists, so the UI trims its catalog chrome (share, counts) and feeds the
  // model picker from the summary's model list instead of the API.
  const bool local = static_cast<bool>(tone->getProperty("local"));
  if (local)
    out->setProperty("local", true);

  // Catalog totals for the model picker's "n/N" and the folder stat (only
  // the active model is stored, so the UI can't count the catalog itself).
  // NAM uses the v2-architecture total; IR and other formats use models_count.
  out->setProperty("models_count", tone->getProperty("models_count"));
  out->setProperty("a2_models_count", tone->getProperty("a2_models_count"));
  // Tone-info stats row: downloads, bookmarks, models (same order as the
  // TONE3000 tone card).
  out->setProperty("downloads_count", tone->getProperty("downloads_count"));
  out->setProperty("favorites_count", tone->getProperty("favorites_count"));
  // Signed-in /tones/{id} sync: whether this user has favorited the tone.
  // Omitted when the stored payload predates the field (signed-out loads,
  // older chains) so the UI treats it as unknown rather than false.
  if (tone->hasProperty("is_favorite"))
    out->setProperty("is_favorite", tone->getProperty("is_favorite"));

  // Canonical public page URL (title slug + id); the UI's share action
  // copies it. Skipped when absent (very old stored tone JSON) so the UI
  // never sees a null; it falls back to the plain /tones/{id} path.
  const juce::String url = tone->getProperty("url").toString();
  if (url.isNotEmpty())
    out->setProperty("url", url);

  // Publish time for the creator line's relative timestamp. Omitted when
  // absent (older stored tones) so the UI skips the "· 3d" suffix.
  const juce::String publishedAt = tone->getProperty("published_at").toString();
  if (publishedAt.isNotEmpty())
    out->setProperty("published_at", publishedAt);

  // Only the first image is ever rendered (block artwork).
  juce::Array<juce::var> images;
  if (auto* imgs = tone->getProperty("images").getArray(); imgs != nullptr && !imgs->isEmpty())
    images.add(imgs->getReference(0));
  out->setProperty("images", images);

  if (auto* user = tone->getProperty("user").getDynamicObject()) {
    juce::DynamicObject::Ptr u = new juce::DynamicObject();
    u->setProperty("username", user->getProperty("username"));
    u->setProperty("avatar_url", user->getProperty("avatar_url"));
    out->setProperty("user", juce::var(u.get()));
  }

  // Catalog tones store only the active model (see parseToneForLoading /
  // switchModel); the picker pages the full catalog from the API
  // client-side. Local tones store all their models, and the switch call
  // needs each one's stash URL (there is no catalog to fetch it from), so
  // for them model_url ships in the summary too.
  juce::Array<juce::var> models;
  if (auto* modelsArr = tone->getProperty("models").getArray()) {
    for (const auto& m : *modelsArr) {
      if (auto* model = m.getDynamicObject()) {
        juce::DynamicObject::Ptr slim = new juce::DynamicObject();
        slim->setProperty("id", model->getProperty("id"));
        slim->setProperty("name", model->getProperty("name"));
        if (local)
          slim->setProperty("model_url", model->getProperty("model_url"));
        models.add(juce::var(slim.get()));
      }
    }
  }
  out->setProperty("models", models);

  return out.get();
}

void TONE3000Processor::setToneOnBlock(ChainBlock& block, int toneId, const juce::String& toneJson,
                                       const juce::var& parsedTone) {
  block.toneId = toneId;
  block.toneJson = toneJson;
  block.toneVar = parsedTone;
  block.toneSummary = makeToneSummary(parsedTone);
}

void TONE3000Processor::queueToneLoad(const std::string& blockId, int modelId,
                                      const juce::String& modelUrl,
                                      const juce::String& modelName, ChainBlockType type) {
  struct LoadToneJob : public juce::ThreadPoolJob {
    TONE3000Processor& processor;
    std::string blockId;
    int modelId;
    juce::String modelUrl;
    juce::String modelName;
    ChainBlockType type;

    LoadToneJob(TONE3000Processor& p, const std::string& bid, int mid, const juce::String& url,
                const juce::String& name, ChainBlockType t)
        : ThreadPoolJob("Load Tone"), processor(p), blockId(bid), modelId(mid), modelUrl(url),
          modelName(name), type(t) {}

    JobStatus runJob() override {
      processor.loadToneInBackground(blockId, modelId, modelUrl, modelName, type);
      return jobHasFinished;
    }
  };

  loadingThreadPool.addJob(new LoadToneJob(*this, blockId, modelId, modelUrl, modelName, type),
                           true);
}

std::string TONE3000Processor::loadTone(const juce::String& toneJsonString,
                                        const std::string& targetInsertId) {
  juce::ScopedLock lock(chainMutex);

  const ParsedTone parsed = parseToneForLoading(toneJsonString);
  if (!parsed.valid)
    return "";

  // Resolve the slot the tone lands in: the insert the user clicked (looked
  // up across the lanes; ids are globally unique), or the active lane's
  // first insert when the id is stale/absent (chain edited mid-flow, or an
  // older UI that doesn't send one). A full grid has no slot to give.
  Lane* targetLane = nullptr;
  size_t slotIndex = 0;
  if (!findLandingSlot(targetInsertId, targetLane, slotIndex))
    return "";

  pushChainHistory();

  // Collision-proof block id (the old 4-digit random ids could collide with
  // long-lived sessions and undo snapshots).
  std::string blockId = juce::Uuid().toString().toStdString();

  auto block = std::make_unique<ChainBlock>(blockId, parsed.type);
  setToneOnBlock(*block, parsed.toneId, parsed.toneJson, parsed.toneVar);
  block->activeModelId = parsed.firstModelId;
  // Fresh blocks load at the machine-wide default A2 size; from here on the
  // size is the block's own (setBlockSlimSize, presets, undo).
  block->processorState.slimSize = namSlimSizeDefault.load();
  block->pushProcessorState();
  block->loaded = false;
  block->modelLoading = true;
  // The only tone-block creation path that doesn't go through
  // applyBlockSettings, so the block's EQ/analyzer meet the chain rate here.
  // An oversampling change while the model is still loading is covered too:
  // prepareChain walks every block in the lane, loading or not.
  prepareBlockForChainRate(*block);
  // The right default mix depends on the model itself (long IR = half wet),
  // which is only known after download; the first successful apply sets it
  // (see applyPreparedModelToChainBlock).
  block->applyDefaultMixOnLoad = true;

  DBG("Created tone block: " << parsed.toneId << " (block: " << blockId << ")");
  DBG("Queueing first model for background loading: " << parsed.modelName);

  // The tone takes the slot's position; the consumed insert dies here (it has
  // no engines, so destroying it under the lock is fine). Alignment then
  // re-pads the lane, which appends a fresh trailing insert once every
  // minimum slot holds a tone, and keeps a branched layout's lane ends even.
  if (slotIndex < targetLane->size())
    (*targetLane)[slotIndex] = std::move(block);
  else
    targetLane->push_back(std::move(block));
  rebuildTopology();

  bumpChainRevision();
  queueToneLoad(blockId, parsed.firstModelId, parsed.modelUrl, parsed.modelName, parsed.type);

  return blockId;
}

std::string TONE3000Processor::landToneBlock(std::unique_ptr<ChainBlock> block, int row,
                                             int index) {
  const std::string newId = block->id;
  Lane& target = lanes[static_cast<size_t>(juce::jlimit(0, kNumLanes - 1, row))];
  index = juce::jlimit(0, static_cast<int>(target.size()), index);
  if (index < static_cast<int>(target.size()) && isInsertBlock(target[static_cast<size_t>(index)])) {
    target[static_cast<size_t>(index)] = std::move(block);  // paste fills the empty tile
  } else if (routingMode == RoutingMode::Grid) {
    // A grid row keeps its eight slots: the splice takes the place of the
    // nearest empty slot, toward the end first. The caller made sure there
    // is one (rowHasEmptySlot).
    int spare = -1;
    for (int i = index; i < static_cast<int>(target.size()) && spare < 0; ++i)
      if (isInsertBlock(target[static_cast<size_t>(i)]))
        spare = i;
    for (int i = juce::jmin(index, static_cast<int>(target.size())) - 1; i >= 0 && spare < 0; --i)
      if (isInsertBlock(target[static_cast<size_t>(i)]))
        spare = i;
    if (spare >= 0) {
      target.erase(target.begin() + spare);
      if (spare < index)
        --index;
    }
    target.insert(target.begin() + juce::jlimit(0, static_cast<int>(target.size()), index),
                  std::move(block));
  } else {
    target.insert(target.begin() + index, std::move(block));
  }
  rebuildTopology();
  refreshBlockTailLength();

  bumpChainRevision();
  queueActiveModelLoad(*findBlockById(newId));
  return newId;
}

std::string TONE3000Processor::duplicateChainBlock(const std::string& sourceBlockId,
                                                   const juce::String& side, int index) {
  const int row = side == "right" ? 1 : 0;
  return duplicateChainBlockToRow(sourceBlockId, row, laneIndexToRowSlot(row, index));
}

// A position in a lane as a legacy client counts it, as a slot of the row.
// They are the same except in the one shape where the client is shown a
// lane that starts later than its row: the branch lane of a grid that is a
// legacy branched rig begins at the split's column.
int TONE3000Processor::laneIndexToRowSlot(int row, int index) const {
  juce::ScopedLock lock(chainMutex);
  if (routingMode != RoutingMode::Grid || row != 1)
    return index;
  const SplitJunction* branch = splitOfRow(0);
  if (branch == nullptr || !demoteBlockersLocked().isEmpty())
    return index;
  return index + branch->splitCol;
}

bool TONE3000Processor::rowTakesBlocks(int row) const noexcept {
  if (row < 0 || row >= kNumLanes)
    return false;
  if (routingMode == RoutingMode::Grid)
    return true;
  return row == 0 || (row == 1 && stereoEnabled.load());
}

// See the declaration. In grid mode a row never grows, so "the first empty
// slot" looks through the rows: the ones that are heard first, then any.
bool TONE3000Processor::findLandingSlot(const std::string& targetInsertId, Lane*& targetLane,
                                        size_t& index) {
  if (!targetInsertId.empty())
    for (auto& l : lanes)
      for (size_t i = 0; i < l.size(); ++i)
        if (isInsertBlock(l[i]) && l[i]->id == targetInsertId) {
          targetLane = &l;
          index = i;
          return true;
        }

  if (routingMode == RoutingMode::Grid) {
    // The row last named for adding (rows 0 and 1, through the legacy lane
    // names), then the rows that are heard, then any.
    const int preferred = pendingAddSide == ChainSide::Right ? 1 : 0;
    for (const int pass : {0, 1, 2})
      for (int row = 0; row < kGridRows; ++row) {
        if (pass == 0 && row != preferred)
          continue;
        if (pass == 1 && !activePlan.rowInfo[static_cast<size_t>(row)].active)
          continue;
        Lane& l = lanes[static_cast<size_t>(row)];
        for (size_t i = 0; i < l.size(); ++i)
          if (isInsertBlock(l[i])) {
            targetLane = &l;
            index = i;
            return true;
          }
      }
    return false;
  }

  targetLane = &activeChain();
  index = static_cast<size_t>(
      std::find_if(targetLane->begin(), targetLane->end(), isInsertBlock) - targetLane->begin());
  return true;
}

std::string TONE3000Processor::duplicateChainBlockToRow(const std::string& sourceBlockId, int row,
                                                        int index) {
  // A hosted plugin is copied from its cached state chunk; bring that up to
  // date first (never under the chain lock).
  serviceHostedPlugins();

  // Structural like reorder/move (a whole new block splices into the running
  // chain), so mute-splice instead of relying on one block's wet fade.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  const ChainBlock* source = findBlockById(sourceBlockId);
  if (source == nullptr || source->type == ChainBlockType::INSERT) {
    DBG("duplicateChainBlock: source not a tone block: " << sourceBlockId);
    return "";
  }
  if (!rowTakesBlocks(row)) {
    DBG("duplicateChainBlock: that lane is not there (the right lane requires stereo mode)");
    return "";
  }
  if (routingMode == RoutingMode::Grid &&
      std::none_of(lanes[static_cast<size_t>(row)].begin(), lanes[static_cast<size_t>(row)].end(),
                   isInsertBlock)) {
    DBG("duplicateChainBlock: the row has no empty slot");
    return "";
  }

  pushChainHistory();

  // The settings ride the same per-block tree the undo/state paths use, so
  // "everything the block remembers" stays defined in exactly one place
  // (serializeBlockSettings/applyBlockSettings: gains, mix, enabled,
  // normalize, A2 size, EQ). Tone identity and model bytes are copied
  // directly: the clone re-loads its model cache-first, so it comes up
  // without a network round trip and sounds identical the moment the engine
  // lands.
  const std::string newId = juce::Uuid().toString().toStdString();
  // Typed by the saved node, not the live block: a registry-kind block with
  // a tone on its way in serialises as the tone block it is becoming.
  const juce::ValueTree sourceSettings = serializeBlockSettings(*source);
  auto clone = std::make_unique<ChainBlock>(
      newId, chainBlockTypeFromString(sourceSettings.getProperty("type").toString()));
  applyBlockSettings(*clone, sourceSettings);
  setToneOnBlock(*clone, source->toneId, source->toneJson, source->toneVar);
  clone->activeModelId = source->activeModelId;
  clone->modelCache = source->modelCache;
  // A registry-kind clone has no model to load: applyBlockSettings queued
  // the build of its processor from the copied KindState and set its
  // loading state (see applyKindState).
  if (clone->type != ChainBlockType::KIND) {
    clone->loaded = false;
    clone->modelLoading = true;
  }
  clone->applyDefaultMixOnLoad = false;  // the copied mix is a setting, not a default

  landToneBlock(std::move(clone), row, index);
  DBG("Duplicated block " << sourceBlockId << " -> " << newId << " (row " << row << " @ " << index
                          << ")");
  return newId;
}

bool TONE3000Processor::copyChainBlock(const std::string& blockId) {
  serviceHostedPlugins();  // see duplicateChainBlock
  juce::ScopedLock lock(chainMutex);

  const ChainBlock* source = findBlockById(blockId);
  if (source == nullptr || source->type == ChainBlockType::INSERT) {
    DBG("copyChainBlock: source not a tone block: " << blockId);
    return false;
  }

  // A self-contained snapshot, not a reference: the same per-block tree the
  // undo/preset paths persist, plus the in-memory model bytes so a later
  // paste comes up offline. Copying never touches the chain, so no history
  // entry; the revision bump only publishes `canPasteBlock` to the UI.
  blockClipboardSettings = serializeBlockSettings(*source);
  blockClipboardModelCache = source->modelCache;

  bumpChainRevision();
  DBG("Copied block " << blockId << " to the block clipboard");
  return true;
}

std::string TONE3000Processor::pasteChainBlock(const juce::String& side, int index) {
  index = laneIndexToRowSlot(side == "right" ? 1 : 0, index);
  // Structural like duplicate: a whole new block lands in the running chain.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (!blockClipboardSettings.isValid()) {
    DBG("pasteChainBlock: clipboard is empty");
    return "";
  }
  const int row = side == "right" ? 1 : 0;
  if (!rowTakesBlocks(row)) {
    DBG("pasteChainBlock: right lane requires stereo mode");
    return "";
  }
  if (routingMode == RoutingMode::Grid &&
      std::none_of(lanes[static_cast<size_t>(row)].begin(), lanes[static_cast<size_t>(row)].end(),
                   isInsertBlock)) {
    DBG("pasteChainBlock: the row has no empty slot");
    return "";
  }

  pushChainHistory();

  // Rebuild from the snapshot the way undo/preset restores do (fresh id: the
  // copied block may still be in the chain, and ids are globally unique).
  // The tone JSON is re-parsed rather than shared so nothing in the live
  // chain can mutate the clipboard behind our back (see switchModel, which
  // edits a block's toneVar in place).
  const std::string newId = juce::Uuid().toString().toStdString();
  const ChainBlockType type =
      chainBlockTypeFromString(blockClipboardSettings.getProperty("type").toString());
  auto block = std::make_unique<ChainBlock>(newId, type);
  applyBlockSettings(*block, blockClipboardSettings);
  const juce::String toneJson = blockClipboardSettings.getProperty("toneJson").toString();
  setToneOnBlock(*block, blockClipboardSettings.getProperty("toneId", 0), toneJson,
                 juce::JSON::parse(toneJson));
  block->activeModelId = blockClipboardSettings.getProperty("activeModelId", 0);
  block->modelCache = blockClipboardModelCache;
  if (block->type != ChainBlockType::KIND) {  // see duplicateChainBlock
    block->loaded = false;
    block->modelLoading = true;
  }
  block->applyDefaultMixOnLoad = false;  // the copied mix is a setting, not a default

  landToneBlock(std::move(block), row, index);
  DBG("Pasted clipboard block -> " << newId << " (" << side << " @ " << index << ")");
  return newId;
}

bool TONE3000Processor::swapTone(const std::string& blockId, const juce::String& toneJsonString) {
  juce::ScopedLock lock(chainMutex);

  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT) {
    DBG("swapTone: block not found or is insert block: " << blockId);
    return false;
  }

  const ParsedTone parsed = parseToneForLoading(toneJsonString);
  if (!parsed.valid)
    return false;

  pushChainHistory();
  // A tone has no kind parameters: scene values of a registry kind this
  // block was running are dropped with it.
  scenes.removeKindParams(block->id);

  // Replace the tone in place: same block id (chain position preserved), same
  // user params (enabled/gains/mix). The old engines (and the block type
  // they belong to) stay live and keep processing until the new model is
  // spliced in by applyPreparedModelToChainBlock (which also stamps the new
  // type); `modelLoading` drives the UI's loading state meanwhile.
  setToneOnBlock(*block, parsed.toneId, parsed.toneJson, parsed.toneVar);
  block->activeModelId = parsed.firstModelId;
  block->modelLoading = true;
  block->loadFailed = false;
  block->modelCache.clear();

  DBG("Swapped tone on block " << blockId << " -> tone " << parsed.toneId);

  bumpChainRevision();
  queueToneLoad(blockId, parsed.firstModelId, parsed.modelUrl, parsed.modelName, parsed.type);

  return true;
}

bool TONE3000Processor::refreshToneMetadata(const juce::String& toneJsonString) {
  const juce::var freshVar = juce::JSON::parse(toneJsonString);
  juce::DynamicObject* fresh = freshVar.getDynamicObject();
  if (fresh == nullptr)
    return false;
  const int toneId = fresh->getProperty("id");
  if (toneId == 0)
    return false;

  juce::ScopedLock lock(chainMutex);

  bool changed = false;
  for (const ChainSide side : {ChainSide::Left, ChainSide::Right}) {
    for (auto& block : lane(side)) {
      if (block->type == ChainBlockType::INSERT || block->toneId != toneId)
        continue;
      // Local tones have no catalog behind them; a same-id API tone is a
      // different thing entirely and must never overwrite one.
      if (static_cast<bool>(block->toneVar["local"]))
        continue;

      // Fresh payload wholesale, except the stored models array: native
      // persists only the active model, and queueActiveModelLoad / retry /
      // switchModel resolve the download URL from that entry. The API
      // payload's models list has no such guarantee.
      juce::var mergedVar = freshVar.clone();
      mergedVar.getDynamicObject()->setProperty("models", block->toneVar["models"]);
      const juce::String mergedJson = juce::JSON::toString(mergedVar);
      if (mergedJson == block->toneJson)
        continue;

      setToneOnBlock(*block, toneId, mergedJson, mergedVar);
      changed = true;
    }
  }

  if (changed) {
    DBG("Refreshed metadata for tone " << toneId);
    bumpChainRevision();
  }
  return changed;
}

bool TONE3000Processor::switchModel(const std::string& blockId, int modelId,
                                    const juce::var& modelData) {
  juce::ScopedLock lock(chainMutex);

  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr) {
    DBG("Block not found: " << blockId);
    return false;
  }

  if (!block->toneVar.isObject()) {
    DBG("Block has no parsed tone metadata");
    return false;
  }

  // Native only stores the *active* model (the catalog lives on the API and
  // the UI pages it in), so the switch always carries the full model object.
  juce::DynamicObject* model = modelData.getDynamicObject();
  if (model == nullptr || static_cast<int>(model->getProperty("id")) != modelId ||
      model->getProperty("model_url").toString().isEmpty()) {
    DBG("switchModel: missing or invalid model data for ID: " << modelId);
    return false;
  }

  const juce::String modelUrl = model->getProperty("model_url").toString();
  const juce::String modelName = model->getProperty("name").toString();

  DBG("Queueing model switch: " << modelName << " (ID: " << modelId << ")");

  pushChainHistory();

  // The new model becomes the tone's sole stored model. Local tones keep
  // their full model list instead (the picked model is already in it; see
  // parseToneForLoading), so only the active id moves.
  if (!static_cast<bool>(block->toneVar["local"])) {
    juce::Array<juce::var> models;
    models.add(modelData);
    block->toneVar.getDynamicObject()->setProperty("models", models);
    block->toneJson = juce::JSON::toString(block->toneVar);
    block->toneSummary = makeToneSummary(block->toneVar);
  }

  // The previous engine keeps processing (loaded stays true) while the new
  // model downloads/prepares; the swap itself is spliced in with a fade.
  block->activeModelId = modelId;
  block->modelLoading = true;
  block->loadFailed = false;
  bumpChainRevision();

  struct SwitchModelJob : public juce::ThreadPoolJob {
    TONE3000Processor& processor;
    std::string blockId;
    int modelId;
    juce::String modelUrl;
    juce::String modelName;

    SwitchModelJob(TONE3000Processor& p, const std::string& bid, int mid,
                  const juce::String& url, const juce::String& name)
        : ThreadPoolJob("Switch Model"), processor(p), blockId(bid), modelId(mid), modelUrl(url),
          modelName(name) {}

    JobStatus runJob() override {
      processor.switchModelInBackground(blockId, modelId, modelUrl, modelName);
      return jobHasFinished;
    }
  };

  loadingThreadPool.addJob(new SwitchModelJob(*this, blockId, modelId, modelUrl, modelName), true);

  return true;
}

juce::var TONE3000Processor::addKindBlock(const juce::String& kindId,
                                          const std::string& targetId) {
  const auto failure = [](const juce::String& message) {
    juce::DynamicObject::Ptr result = new juce::DynamicObject();
    result->setProperty("error", message);
    return juce::var(result.get());
  };

  const BlockKindInfo* info = BlockKindRegistry::instance().find(kindId);
  if (info == nullptr)
    return failure("unknown block kind '" + kindId + "'");
  if (!info->addable)
    return failure("a '" + kindId + "' block is added by loading a tone, not by kind");

  // Built and prepared out here, on the caller's thread and off the chain
  // lock, like a model: construction may be slow and may allocate. The
  // install step repairs any drift against the live domain.
  PreparedBlockModel prepared;
  prepared.processor = info->factory();
  if (prepared.processor == nullptr)
    return failure("could not create a '" + kindId + "' block");
  prepared.processor->prepareForChain(blockPrepareSpecForId(targetId));
  prepared.success = true;

  // Which fade the splice needs. A block with latency (the new one, or the
  // one being replaced) moves its lane in time, so the whole chain is
  // muted around it; otherwise a replaced block mutes its own wet path, and
  // a fresh one needs nothing up front (it fades in from bypass).
  bool replacing = false;
  bool timingMoves = prepared.processor->chainLatencySamples() > 0;
  if (!targetId.empty()) {
    juce::ScopedLock lock(chainMutex);
    const ChainBlock* target = findBlockById(targetId);
    if (target == nullptr)
      return failure("no block with id '" + juce::String(targetId) + "'");
    replacing = target->type != ChainBlockType::INSERT;
    timingMoves = timingMoves || target->dryDelay.delaySamples() > 0;
  }

  std::optional<ChainEditFade> editFade;
  if (timingMoves)
    editFade.emplace(*this);
  else if (replacing)
    requestSwapFadeAndWait(targetId, /*muteWetOnly=*/true);

  std::string blockId;
  {
    juce::ScopedLock lock(chainMutex);

    ChainBlock* block = nullptr;
    if (replacing) {
      block = findBlockById(targetId);
      if (block == nullptr || block->type == ChainBlockType::INSERT)
        return failure("block '" + juce::String(targetId) + "' changed while the kind was built");
      pushChainHistory();
      // Scene values of the outgoing kind's parameters mean nothing to the
      // new one (the block's on/off states and common settings stay).
      scenes.removeKindParams(block->id);
      // The block keeps its id, position, gains, mix and EQ; what made it a
      // tone goes. Clearing the active model also retires any model load
      // still in flight for it (the loaders check it before they install).
      setToneOnBlock(*block, 0, {}, {});
      block->activeModelId = 0;
      block->modelCache.clear();
      block->modelLoading = false;
      block->loadFailed = false;
      block->applyDefaultMixOnLoad = false;
      ++block->kindBuildSerial;  // and any kind build it was waiting for
    } else {
      // The same slot resolution as loadTone: the named insert, else the
      // active lane's first one, else the lane's end (never in a grid row,
      // which keeps its eight slots).
      Lane* targetLane = nullptr;
      size_t slotIndex = 0;
      if (!findLandingSlot(targetId, targetLane, slotIndex))
        return failure("the grid has no empty slot");

      pushChainHistory();
      auto fresh = std::make_unique<ChainBlock>(juce::Uuid().toString().toStdString(),
                                                ChainBlockType::KIND);
      prepareBlockForChainRate(*fresh);
      // A new block starts at its kind's mix (a delay sits next to the dry
      // signal, a gain replaces it). A replaced block keeps the user's.
      fresh->mixNormalized = info->defaultMix;
      block = fresh.get();

      if (slotIndex < targetLane->size())
        (*targetLane)[slotIndex] = std::move(fresh);
      else
        targetLane->push_back(std::move(fresh));
    }

    blockId = block->id;
    // Loaded, stamped with its kind, armed to fade in (or back in, after a
    // replace's wet mute), and counted in the tail.
    applyPreparedModelToChainBlock(*block, ChainBlockType::KIND, prepared);
    // Lane bookkeeping, and with it the latency compensation.
    rebuildTopology();
    bumpChainRevision();
  }
  // `prepared` now holds a replaced block's previous processor; it dies
  // here, after the lock.

  juce::DynamicObject::Ptr result = new juce::DynamicObject();
  result->setProperty("blockId", juce::String(blockId));
  return juce::var(result.get());
}

bool TONE3000Processor::removeChainBlock(const std::string& blockId) {
  // Removal is bypass, so glide the block's wet mix to bypass first; then
  // detaching it is inaudible. Bounded wait (~one fade; skipped when audio
  // is stopped), on the message thread, imperceptible for a click gesture.
  // A block with latency is the exception: its bypass is the delayed signal,
  // and taking it out moves the lane in time, so the whole chain is muted
  // around the removal instead.
  std::optional<ChainEditFade> editFade;
  if (blockLatencySamples(blockId) > 0)
    editFade.emplace(*this);
  else
    requestSwapFadeAndWait(blockId);

  // Detached under the lock, destroyed after releasing it: engine teardown
  // (NAM graph, convolution state) is heavy and the audio thread may be
  // waiting on chainMutex.
  std::unique_ptr<ChainBlock> removed;
  {
    juce::ScopedLock lock(chainMutex);

    for (auto& chain : lanes) {
      auto it = std::find_if(
          chain.begin(), chain.end(),
          [&blockId](const std::unique_ptr<ChainBlock>& block) { return block->id == blockId; });
      if (it != chain.end()) {
        if (isInsertBlock(*it)) {
          DBG("Cannot remove insert block");
          return false;
        }
        pushChainHistory();
        removed = std::move(*it);
        if (routingMode == RoutingMode::Grid)
          // The slot empties in place: a grid's columns do not shift.
          *it = std::make_unique<ChainBlock>(juce::Uuid().toString().toStdString(),
                                             ChainBlockType::INSERT);
        else
          chain.erase(it);
        // What the scenes and the footswitches held for it goes with it (an
        // undo brings both back: they are in the snapshot just pushed).
        scenes.removeBlock(blockId);
        stomps.removeBlock(blockId);
        // Dropping below the minimum grows the lane back to it (at the end);
        // removing the tapped block clears the branch, and a shortened trunk
        // re-aligns the branch lane's end.
        rebuildTopology();
        refreshBlockTailLength();  // a long-tailed IR may just have left the chain
        bumpChainRevision();
        break;
      }
    }
  }

  if (removed == nullptr) {
    DBG("Failed to remove chain block: " << blockId << " (not found)");
    return false;
  }

  DBG("Removed chain block: " << blockId);
  return true;
}

bool TONE3000Processor::reorderChainBlocks(const std::vector<std::string>& newOrder) {
  // A grid row reorders in place when the order names all of its slots. A
  // legacy client that is shown a grid as a legacy rig names the lane it was
  // shown, which is shorter than the row: the rig then goes back to being
  // that legacy rig first, and the order applies to its lane.
  {
    bool wholeRow = !isGridMode();
    if (!wholeRow) {
      juce::ScopedLock lock(chainMutex);
      std::vector<std::string> wanted = newOrder;
      std::sort(wanted.begin(), wanted.end());
      for (const auto& l : lanes) {
        std::vector<std::string> ids;
        for (const auto& block : l)
          ids.push_back(block->id);
        std::sort(ids.begin(), ids.end());
        wholeRow = wholeRow || (!ids.empty() && ids == wanted);
      }
    }
    if (!wholeRow && !leaveGridForLaneEdit())
      return false;
  }

  // Reordering nonlinear blocks changes the chain's waveform discontinuously
  // (no single block to fade), so mute-splice: glide the chain output to
  // silence, apply, glide back (~25 ms each way; see ChainEditFade).
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  // Both lanes render at once now, so the target chain is inferred from the
  // ids themselves: the order must be a permutation of exactly one lane.
  // (Block ids are globally unique across both chains.)
  auto isPermutationOf = [](const std::vector<std::unique_ptr<ChainBlock>>& chain,
                            const std::vector<std::string>& order) {
    if (order.size() != chain.size())
      return false;
    std::vector<std::string> chainIds, orderIds = order;
    for (const auto& block : chain)
      chainIds.push_back(block->id);
    std::sort(chainIds.begin(), chainIds.end());
    std::sort(orderIds.begin(), orderIds.end());
    return chainIds == orderIds;
  };

  std::vector<std::unique_ptr<ChainBlock>>* target = nullptr;
  for (auto& l : lanes)
    if (isPermutationOf(l, newOrder)) {
      target = &l;
      break;
    }

  if (target == nullptr) {
    DBG("Failed to reorder chain blocks: order is not a permutation of either chain");
    return false;
  }
  auto& chain = *target;

  pushChainHistory();

  std::vector<std::unique_ptr<ChainBlock>> reorderedBlocks;
  reorderedBlocks.reserve(chain.size());
  std::vector<std::unique_ptr<ChainBlock>> originalBlocks = std::move(chain);

  for (const std::string& blockId : newOrder) {
    auto it = std::find_if(
        originalBlocks.begin(), originalBlocks.end(),
        [&blockId](const std::unique_ptr<ChainBlock>& block) {
          return block && block->id == blockId;
        });
    reorderedBlocks.push_back(std::move(*it));
  }

  chain = std::move(reorderedBlocks);
  // The tap follows its block to the new position (a moved tap changes the
  // branch lane's indent, so its trailing inserts re-align).
  rebuildTopology();
  bumpChainRevision();
  DBG("Successfully reordered chain blocks (including insert block)");
  return true;
}

bool TONE3000Processor::moveBlockToChain(const std::string& blockId, const juce::String& side,
                                         int index) {
  // The legacy cross-lane drag, by position in a lane. A grid that is a
  // legacy rig in shape goes back to being one first; any other grid moves
  // blocks with moveGridBlock.
  if (!leaveGridForLaneEdit())
    return false;

  // Cross-lane moves change both chains at once; mute-splice like reorder.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (!stereoEnabled.load()) {
    DBG("moveBlockToChain: only valid in stereo mode");
    return false;
  }

  auto& target = lane(side == "right" ? ChainSide::Right : ChainSide::Left);
  auto& source = lane(side == "right" ? ChainSide::Left : ChainSide::Right);

  auto it = std::find_if(source.begin(), source.end(),
                         [&blockId](const std::unique_ptr<ChainBlock>& block) {
                           return block && block->id == blockId;
                         });
  if (it == source.end()) {
    DBG("moveBlockToChain: block not found in the other lane: " << blockId);
    return false;
  }
  if (isInsertBlock(*it)) {
    DBG("moveBlockToChain: insert slots stay in their lane");
    return false;
  }

  pushChainHistory();

  auto block = std::move(*it);
  source.erase(it);
  index = juce::jlimit(0, static_cast<int>(target.size()), index);
  target.insert(target.begin() + index, std::move(block));

  // The tone count changed on both sides: the source may need a slot back,
  // the target may shed a (trailing) surplus one. The tapped block leaving
  // the trunk clears the branch; otherwise the lane ends re-align.
  rebuildTopology();

  bumpChainRevision();
  DBG("Moved block " << blockId << " to " << side << " chain at index " << index);
  return true;
}

// A background download/prepare failed: leave the block unloaded but flip
// loadFailed (with a revision bump) so the UI swaps its loading dots for a
// retry affordance instead of spinning forever.
void TONE3000Processor::markBlockLoadFailed(const std::string& blockId) {
  // The previous engine kept playing during the download; the UI already
  // shows the new tone/model, so on failure the block drops out of
  // processing to match, glided to bypass first, never spliced.
  requestSwapFadeAndWait(blockId);

  juce::ScopedLock lock(chainMutex);
  if (ChainBlock* block = findBlockById(blockId)) {
    // The block became a registry kind while this model was loading (see
    // addKindBlock): the load is stale and its failure is nobody's problem.
    if (block->type == ChainBlockType::KIND && block->activeModelId == 0) {
      block->swapFadePending.store(false);
      return;
    }
    juce::Logger::writeToLog("[ModelLoader] Load failed for block " + juce::String(blockId) +
                             ", showing retry");
    block->loaded = false;
    block->loadFailed = true;
    block->modelLoading = false;
    block->swapFadePending.store(false);  // never leave the block faded out
    bumpChainRevision();
  }
}

// Re-queue the block's active model (retry after a failed download). The
// background loader is cache-first, so this only hits the network for the
// bytes that actually failed to arrive.
bool TONE3000Processor::retryModelLoad(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  // Only a model load can be retried. A registry kind has none, unless a
  // tone was being swapped onto it when the download failed.
  if (block == nullptr || block->type == ChainBlockType::INSERT ||
      (block->type == ChainBlockType::KIND && !block->hasToneOnKind()) || !block->loadFailed)
    return false;

  block->loadFailed = false;
  block->modelLoading = true;
  bumpChainRevision();  // back to the loading state in the UI
  queueActiveModelLoad(*block);
  return true;
}

void TONE3000Processor::loadToneInBackground(const std::string& blockId, int firstModelId,
                                             const juce::String& modelUrl,
                                             const juce::String& modelName, ChainBlockType type) {
  DBG("[Background] Loading tone for block: " << blockId);

  std::vector<uint8_t> modelData = fetchModelFromUrl(modelUrl);
  if (modelData.empty()) {
    DBG("[Background] Failed to fetch model from URL");
    markBlockLoadFailed(blockId);
    return;
  }

  const juce::String filename =
      modelName + (type == ChainBlockType::NAM ? ".nam" : ".wav");

  double namSlimSize = 0.0;
  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      juce::Logger::writeToLog("[Background] Tone load dropped, block not found: " +
                               juce::String(blockId));
      return;
    }
    // Superseded while the bytes were on their way (a newer swap, an undo,
    // the block turned into a registry kind): nothing of this load belongs
    // on the block any more, its bytes included.
    if (block->activeModelId != firstModelId)
      return;

    block->modelCache[firstModelId] = modelData;
    namSlimSize = block->processorState.slimSize;
  }

  // The voice count is the block's row's, read as late as the build allows
  // (the apply step re-queues the block if it moved all the same).
  PreparedBlockModel prepared = prepareBlockModelOffThread(
      type, modelData, filename, namSlimSize, blockPrepareSpecForId(blockId).voiceCount);
  const bool applied = prepared.success;

  // A swapped tone's previous engine may still be audibly processing; let
  // the audio thread fade it out before the outcome is applied. On success
  // the wet path mutes in place (the dry input is never exposed, see
  // ChainBlock.h); on failure the block is dropped from processing, so it
  // glides to bypass, which is what plays afterwards. No fade at all for a
  // load the block no longer wants, and the whole chain's when the outgoing
  // processor has latency (see fadeForModelInstall).
  const auto editFade = fadeForModelInstall(blockId, firstModelId, prepared.success);

  {
    juce::ScopedLock lock(chainMutex);

    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      DBG("[Background] Block not found after prepare: " << blockId);
      return;
    }

    if (block->activeModelId != firstModelId) {
      // Superseded by a newer switch/swap while this one loaded; that job
      // owns the block's loading state now; just make sure the block isn't
      // left faded out.
      block->swapFadePending.store(false);
      return;
    }

    applyPreparedModelToChainBlock(*block, type, prepared);
  }
  // `prepared` now holds the block's *previous* engines (if any); they are
  // destroyed here, after the lock; teardown is too heavy to hold it.

  if (applied) {
    DBG("[Background] Successfully loaded tone for block: " << blockId);
  }
}

void TONE3000Processor::switchModelInBackground(const std::string& blockId, int modelId,
                                                const juce::String& modelUrl,
                                                const juce::String& modelName) {
  DBG("[Background] Switching model for block: " << blockId << " to model ID: " << modelId);

  std::vector<uint8_t> modelData;
  bool needsFetch = false;
  ChainBlockType blockTypeForPrepare = ChainBlockType::NAM;
  double namSlimSize = 0.0;

  {
    juce::ScopedLock lock(chainMutex);

    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      juce::Logger::writeToLog("[Background] Load dropped, block not found: " + juce::String(blockId));
      return;
    }

    if (block->activeModelId != modelId) {
      juce::Logger::writeToLog("[Background] Load for model " + juce::String(modelId) +
                               " superseded before it started (block " + juce::String(blockId) + ")");
      return;
    }

    // Key the prepare off the *tone's* format, not block->type: during an
    // in-flight tone swap the block keeps its previous type (that engine is
    // still processing) while this job builds the new tone's engine.
    blockTypeForPrepare = toneEngineType(block->toneVar, block->type);
    namSlimSize = block->processorState.slimSize;
    auto cacheIt = block->modelCache.find(modelId);

    if (cacheIt != block->modelCache.end()) {
      DBG("[Background] Using cached model data");
      modelData = cacheIt->second;
    } else {
      needsFetch = true;
    }
  }

  if (needsFetch) {
    DBG("[Background] Fetching model from URL: " << modelUrl);
    modelData = fetchModelFromUrl(modelUrl);

    if (modelData.empty()) {
      DBG("[Background] Failed to fetch model from URL");
      markBlockLoadFailed(blockId);
      return;
    }
  } else {
    // Local-model stash upkeep for cache-hit loads (fetches do their own in
    // fetchModelFromUrl); no-op for catalog URLs.
    refreshLocalStashCopy(modelUrl, modelData);
  }

  const juce::String filename =
      modelName + (blockTypeForPrepare == ChainBlockType::NAM ? ".nam" : ".wav");

  PreparedBlockModel prepared =
      prepareBlockModelOffThread(blockTypeForPrepare, modelData, filename, namSlimSize,
                                 blockPrepareSpecForId(blockId).voiceCount);
  const bool applied = prepared.success;

  // The outgoing model keeps processing until this moment; fade it out on
  // the audio thread so the outcome can't click. On success the wet path
  // mutes in place (an engine swap must never expose the block's dry input:
  // at 100% mix that's a burst of the un-cabbed/un-ampped signal, see
  // ChainBlock.h); on a failed prepare the block is dropped, so it glides
  // to bypass instead. (No fade for a superseded load, the whole chain's
  // for a block leaving latency behind: see fadeForModelInstall.)
  const auto editFade = fadeForModelInstall(blockId, modelId, prepared.success);

  {
    juce::ScopedLock lock(chainMutex);

    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr) {
      DBG("[Background] Block was removed during fetch/install");
      return;
    }

    // Kept even when a newer switch superseded this one (the bytes are
    // still this tone's), but not on a block that has stopped being a tone.
    if (needsFetch && !(block->type == ChainBlockType::KIND && block->activeModelId == 0)) {
      block->modelCache[modelId] = modelData;
    }

    if (block->activeModelId != modelId) {
      // Superseded by a newer switch/swap while this one downloaded; that
      // job owns the block's loading state now. The bytes stay cached above.
      block->swapFadePending.store(false);
      return;
    }

    applyPreparedModelToChainBlock(*block, blockTypeForPrepare, prepared);
  }
  // `prepared` now holds the block's *previous* engines (if any); they are
  // destroyed here, after the lock; teardown is too heavy to hold it.

  if (applied) {
    DBG("[Background] Successfully switched to model ID: " << modelId);
  }
}

// Promote a settled continuous gesture (knob/EQ drag) into a real revision
// bump. Mid-gesture edits only record a timestamp (deferredRevisionBump);
// once the gesture has been quiet for kGestureSettleMs the next revision
// check converges everyone on the final values with a single full resync.
// Called by the editor's push timer and by getChainState.
juce::uint32 TONE3000Processor::getCurrentChainRevision() const {
  if (const auto pendingAt = pendingParamBumpAt.load(); pendingAt != 0 &&
      juce::Time::currentTimeMillis() - pendingAt >= kGestureSettleMs) {
    pendingParamBumpAt.store(0);
    bumpChainRevision();
  }
  return chainRevision.load();
}

juce::var TONE3000Processor::getChainState(int knownRevision) const {
  // Per-block copy of everything the payload needs. juce::String and
  // juce::var are refcounted, so a row copy is a handful of pointer bumps.
  // I deliberately defer the DynamicObject building (hash-map inserts, many
  // small allocations) until after chainMutex is released, so a UI resync
  // can't stall the audio thread on lock contention.
  struct BlockRow {
    juce::String id;
    bool isInsert = false;
    juce::var toneSummary;
    int toneId = 0;
    int activeModelId = 0;
    bool loaded = false, loadFailed = false, modelLoading = false, irLong = false;
    bool hasInputDbu = false, hasOutputDbu = false;
    double inputDbu = 0.0, outputDbu = 0.0;
    bool enabled = true, normalize = true;
    double slimSize = 0.0;
    float inputGain = 0.5f, outputGain = 0.5f, mix = 1.0f;
    juce::var eq;
    bool rtFailed = false;
    // Kind identity, for every tone block (see the payload below).
    ChainBlockType type = ChainBlockType::NAM;
    juce::String kindId, kindName;
    bool missingKind = false, processingFailed = false;
    std::vector<std::pair<juce::String, double>> kindParams;
    // A hosted plugin block: which plugin (see HostedPluginIdentity::toVar).
    juce::var plugin;
    // Parameter ids with a value per scene, and those flagged for the
    // expression pedal (see SceneStore).
    juce::StringArray sceneParams, expressionParams;
  };

  juce::uint32 revision = 0;
  std::vector<BlockRow> left, right;
  // Grid mode: every row's blocks (rows 0 and 1 are `left` and `right`),
  // the grid's own description, and whether a legacy rig could express it.
  std::array<std::vector<BlockRow>, kGridRows> gridRows;
  bool gridMode = false, gridRightShown = false, legacyEditable = true;
  bool projectedDualMono = false;
  juce::var gridState;
  juce::String projectedInputMode;
  bool stereo = false, canUndo = false, canRedo = false, branched = false;
  bool canPaste = false, atDefault = false;
  int activeScene = 0;
  juce::String presetId, presetName, branchSide, activeSide, branchAfter;

  {
    juce::ScopedLock lock(chainMutex);

    // Read under the lock so the revision always matches the snapshot:
    // mutators bump the revision while holding chainMutex too. (Also promotes
    // any settled gesture edit into a bump.)
    revision = getCurrentChainRevision();

    // Cheap early-out for the UI poll loop when nothing changed.
    if (knownRevision >= 0 && static_cast<juce::uint32>(knownRevision) == revision) {
      juce::DynamicObject::Ptr unchanged = new juce::DynamicObject();
      unchanged->setProperty("revision", static_cast<int>(revision));
      unchanged->setProperty("unchanged", true);
      return unchanged.get();
    }

    // A lane as a list of its blocks: a legacy lane or a grid row whole, or
    // the part of a row a legacy client is shown.
    const auto blocksOf = [](const Lane& l) {
      std::vector<ChainBlock*> blocks;
      blocks.reserve(l.size());
      for (const auto& block : l)
        blocks.push_back(block.get());
      return blocks;
    };
    auto copyLane = [this](const std::vector<ChainBlock*>& l, std::vector<BlockRow>& out) {
      out.reserve(l.size());
      for (ChainBlock* block : l) {
        BlockRow row;
        row.id = juce::String(block->id);
        // Drain the audio thread's failure flag; the RT path can't build
        // strings or write logs, so it gets reported below, outside the lock.
        row.rtFailed = block->rtProcessingFailed.exchange(false);
        if (row.rtFailed)
          block->processingFailed = true;
        if (block->type == ChainBlockType::INSERT) {
          row.isInsert = true;
          out.push_back(std::move(row));
          continue;
        }
        row.toneSummary = block->toneSummary;
        row.toneId = block->toneId;
        row.activeModelId = block->activeModelId;
        row.loaded = block->loaded;
        row.loadFailed = block->loadFailed;
        row.modelLoading = block->modelLoading;
        const IrBlockProcessor* ir = block->ir();
        row.irLong = ir != nullptr && ir->isLong();
        // NAM calibration metadata off the loaded engine, absent when the
        // model carries none. Non-finite values never ship: the DSP rejects
        // them and JSON (presets) can't carry them.
        const NamBlockProcessor* nam = block->nam();
        if (const NamEngine* engine = nam != nullptr ? nam->engine() : nullptr) {
          if (engine->hasInputLevel() && std::isfinite(engine->getInputLevel())) {
            row.hasInputDbu = true;
            row.inputDbu = engine->getInputLevel();
          }
          if (engine->hasOutputLevel() && std::isfinite(engine->getOutputLevel())) {
            row.hasOutputDbu = true;
            row.outputDbu = engine->getOutputLevel();
          }
        }
        row.enabled = block->enabled;
        row.normalize = block->processorState.normalize;
        row.slimSize = block->processorState.slimSize;
        row.inputGain = block->inputGainNormalized;
        row.outputGain = block->outputGainNormalized;
        row.mix = block->mixNormalized;
        row.eq = block->eq.toVar();
        row.type = block->type;
        row.processingFailed = block->processingFailed;
        row.missingKind = block->isMissingKind();
        if (block->type == ChainBlockType::KIND) {
          row.kindId = block->kindId;
          row.kindName = block->kindName;
          if (block->processor != nullptr) {
            const BlockParamTable& table = block->processor->parameterTable();
            row.kindParams.reserve(table.size());
            for (size_t i = 0; i < table.size(); ++i)
              row.kindParams.emplace_back(
                  table[i].id,
                  static_cast<double>(block->processor->getParameter(static_cast<int>(i))));
          }
          if (block->kindId == HostedPluginBlockProcessor::kKindId) {
            // From the running instance, else from the node the block is
            // loading from or is a placeholder for.
            if (const auto* hosted = HostedPluginBlockProcessor::from(block->processor.get())) {
              row.plugin = hosted->identity().toVar();
              // The layout the plugin runs in, once it exists.
              row.plugin.getDynamicObject()->setProperty("inputChannels",
                                                         hosted->instance()->inputChannels());
              row.plugin.getDynamicObject()->setProperty("outputChannels",
                                                         hosted->instance()->outputChannels());
            } else
              row.plugin =
                  HostedPluginBlockProcessor::identityOf(
                      block->pendingKindState.isValid()
                          ? block->pendingKindState
                          : block->missingKindNode.getChildWithName("KindState"))
                      .toVar();
          }
        } else {
          row.kindId = chainBlockTypeToString(block->type);
        }
        for (const auto& entry : scenes.assigned)
          if (entry.first.blockId == block->id)
            row.sceneParams.add(entry.first.paramId);
        for (const auto& key : scenes.expression)
          if (key.blockId == block->id)
            row.expressionParams.add(key.paramId);
        out.push_back(std::move(row));
      }
    };

    stereo = stereoEnabled.load();
    gridMode = routingMode == RoutingMode::Grid;
    if (gridMode) {
      // What a client that only knows the legacy rig is given of a grid.
      // Every row's blocks ride in the
      // grid object either way.
      for (int row = 0; row < kGridRows; ++row)
        copyLane(blocksOf(lanes[static_cast<size_t>(row)]), gridRows[static_cast<size_t>(row)]);
      gridState = gridStateLocked();
      const LegacyProjection projection = projectGridToLegacy();
      legacyEditable = projection.projectable;
      if (projection.projectable) {
        // One of the legacy shapes: exactly the state the legacy rig it
        // converts to would report. The lanes list the row's blocks and as
        // many of its empty slots as a legacy lane keeps.
        copyLane(projection.left, left);
        stereo = projection.stereo;
        gridRightShown = projection.stereo;
        if (projection.stereo)
          copyLane(projection.right, right);
        projectedInputMode = inputModeToString(projection.inputMode);
        projectedDualMono = projection.dualMono;
        branchSide = projection.branchSide;
        branchAfter = projection.branchAfterBlockId;
      } else {
        // Not a legacy shape: row 0 as the left chain and, when it runs,
        // row 1 as the right one, all eight slots of each, and no branch.
        // Rows 2 and 3 are in the grid object only.
        left = gridRows[0];
        gridRightShown = activePlan.rowInfo[1].active;
        stereo = gridRightShown;
        if (gridRightShown)
          right = gridRows[1];
        // Reported once, by the grid's own lists.
        for (auto* copied : {&left, &right})
          for (auto& row : *copied)
            row.rtFailed = false;
        projectedInputMode = "grid";
      }
    } else {
      copyLane(blocksOf(lane(ChainSide::Left)), left);
      if (stereo)
        copyLane(blocksOf(lane(ChainSide::Right)), right);
    }

    canUndo = chainHistory.canUndo();
    canRedo = chainHistory.canRedo();
    canPaste = blockClipboardSettings.isValid();
    atDefault = isChainAtDefault();
    activeScene = scenes.active;
    presetId = activePresetId;
    presetName = activePresetName;
    if (gridMode) {
      branched = branchAfter.isNotEmpty();  // from the projection
    } else {
      branched = stereo && !branchAfterBlockId.empty();
      if (branched) {
        branchSide = branchSourceSide == ChainSide::Right ? "right" : "left";
        branchAfter = juce::String(branchAfterBlockId);
      }
    }
    activeSide = pendingAddSide == ChainSide::Right ? "right" : "left";
  }

  // Lock released; build the payload.
  auto serializeChain = [](const std::vector<BlockRow>& rows) {
    juce::Array<juce::var> chainArray;
    for (const auto& row : rows) {
      // The same line for every kind: which block, and what it was running.
      if (row.rtFailed) {
        const BlockKindInfo* failedKind = BlockKindRegistry::instance().find(row.kindId);
        juce::Logger::writeToLog("[Block] Processing failed for block " + row.id + " (" +
                                 (failedKind != nullptr ? failedKind->name : row.kindId) +
                                 "); block disabled");
      }

      juce::DynamicObject::Ptr item = new juce::DynamicObject();
      item->setProperty("blockId", row.id);

      if (row.isInsert) {
        item->setProperty("kind", "insert");
        chainArray.add(juce::var(item.get()));
        continue;
      }

      item->setProperty("kind", "tone");

      // Slim tone summary, nested (not spread) so runtime fields never
      // collide with tone fields. Built once when the tone was set (see
      // makeToneSummary) and shipped by reference.
      juce::var toneVar = row.toneSummary;
      if (row.type == ChainBlockType::KIND && row.activeModelId == 0) {
        // A registry-kind block has no tone (one that has a tone on its way
        // in, see swapTone, already shows that tone). It still ships as a "tone"
        // item with a stand-in summary, so a client written against tone
        // blocks (the native UI) draws an ordinary tile named after the
        // kind instead of mistaking the block for an empty slot. `local`
        // keeps such a client away from the catalog (there is nothing to
        // fetch for id 0); clients that know about kinds read kindId.
        const BlockKindInfo* kindInfo = BlockKindRegistry::instance().find(row.kindId);
        juce::DynamicObject::Ptr standIn = new juce::DynamicObject();
        standIn->setProperty("id", 0);
        standIn->setProperty("title", row.kindName.isNotEmpty() ? row.kindName : row.kindId);
        standIn->setProperty("format", row.kindId);
        standIn->setProperty("gear", kindInfo != nullptr
                                         ? blockCategoryToString(kindInfo->category)
                                         : juce::String());
        standIn->setProperty("local", true);
        standIn->setProperty("images", juce::Array<juce::var>());
        standIn->setProperty("models", juce::Array<juce::var>());
        toneVar = juce::var(standIn.get());
      } else if (!toneVar.isObject()) {
        // Corrupt toneJson: emit a minimal stand-in so the UI's `tone`
        // field is always an object.
        juce::DynamicObject::Ptr fallback = new juce::DynamicObject();
        fallback->setProperty("id", row.toneId);
        fallback->setProperty("title", "Tone " + juce::String(row.toneId));
        fallback->setProperty("models", juce::Array<juce::var>());
        toneVar = juce::var(fallback.get());
      }
      item->setProperty("tone", toneVar);

      item->setProperty("activeModelId", row.activeModelId);
      item->setProperty("loaded", row.loaded);
      item->setProperty("loadFailed", row.loadFailed);
      item->setProperty("modelLoading", row.modelLoading);
      // Long (reverb-like) IR: drives the UI's Mix knob default/Alt-click
      // reset and the Out knob help (long IRs carry no -18 dB pad).
      item->setProperty("irLong", row.irLong);

      // What the block is, for every kind: its registry id, a category to
      // sort and colour by, and the kind's own parameter values in real
      // units ({} for the model-based kinds). The model-based kinds refine
      // the registry's category per block: a pedal capture is a drive, a
      // long IR a reverb.
      {
        const BlockKindInfo* kindInfo = BlockKindRegistry::instance().find(row.kindId);
        juce::String category =
            kindInfo != nullptr ? blockCategoryToString(kindInfo->category) : juce::String();
        if (row.type == ChainBlockType::NAM &&
            row.toneSummary["gear"].toString().equalsIgnoreCase("pedal"))
          category = blockCategoryToString(BlockCategory::Drive);
        else if (row.type == ChainBlockType::IR && row.irLong)
          category = blockCategoryToString(BlockCategory::Reverb);
        item->setProperty("kindId", row.kindId);
        item->setProperty("category", category);
        juce::DynamicObject::Ptr kindParams = new juce::DynamicObject();
        for (const auto& [paramId, value] : row.kindParams)
          kindParams->setProperty(juce::Identifier(paramId), value);
        item->setProperty("kindParams", juce::var(kindParams.get()));
        if (row.plugin.isObject())
          item->setProperty("plugin", row.plugin);
      }
      // A kind this build does not have: the block is a placeholder that
      // passes audio through until the kind is available again.
      if (row.missingKind)
        item->setProperty("missing", true);
      // The processor failed while running and the block was taken out of
      // processing (see BlockProcessResult::Failed).
      if (row.processingFailed)
        item->setProperty("processingFailed", true);

      if (row.hasInputDbu)
        item->setProperty("inputLevelDbu", row.inputDbu);
      if (row.hasOutputDbu)
        item->setProperty("outputLevelDbu", row.outputDbu);

      juce::DynamicObject::Ptr params = new juce::DynamicObject();
      params->setProperty("enabled", row.enabled);
      params->setProperty("normalize", row.normalize);
      params->setProperty("slimSize", row.slimSize);
      params->setProperty("inputGain", row.inputGain);
      params->setProperty("outputGain", row.outputGain);
      params->setProperty("mix", row.mix);
      params->setProperty("eq", row.eq);
      item->setProperty("params", juce::var(params.get()));

      // Which of the block's parameters hold a value per scene (the block's
      // on/off state always does), and which are set aside for the
      // expression pedal. The values themselves are in scene.get.
      juce::Array<juce::var> sceneParams, expressionParams;
      for (const auto& paramId : row.sceneParams)
        sceneParams.add(paramId);
      for (const auto& paramId : row.expressionParams)
        expressionParams.add(paramId);
      item->setProperty("sceneParams", sceneParams);
      item->setProperty("expressionParams", expressionParams);

      chainArray.add(juce::var(item.get()));
    }
    return chainArray;
  };

  juce::DynamicObject::Ptr state = new juce::DynamicObject();
  state->setProperty("revision", static_cast<int>(revision));
  const juce::var leftItems = serializeChain(left);
  state->setProperty("chain", leftItems);
  if (gridMode) {
    // A grid rig in the legacy shape, for a client that only knows that
    // one, plus the grid itself with every row's blocks. legacyEditable
    // says whether the legacy routing edits still apply (they convert the
    // rig back first); when it is false they are refused.
    if (gridRightShown)
      state->setProperty("chainRight", serializeChain(right));
    // The failure flags were drained into the rows copied first, so the
    // grid's own lists are what reports them.
    if (const auto* rows = gridState["rows"].getArray())
      for (int row = 0; row < rows->size() && row < kGridRows; ++row)
        if (auto* rowObject = rows->getReference(row).getDynamicObject())
          rowObject->setProperty("blocks", serializeChain(gridRows[static_cast<size_t>(row)]));
    state->setProperty("grid", gridState);
    state->setProperty("legacyEditable", legacyEditable);
  } else if (stereo) {
    state->setProperty("chainRight", serializeChain(right));
  }
  // History flags ride along with the chain state: they only ever change
  // together with a revision bump (mutation, undo/redo or a state load).
  state->setProperty("canUndo", canUndo);
  state->setProperty("canRedo", canRedo);
  // Whether the in-app block clipboard holds a copied block (Paste enabled
  // on insert slots). The clipboard snapshot is self-contained, so this
  // stays true across preset switches and after the source block is gone.
  state->setProperty("canPasteBlock", canPaste);
  // True when nothing distinguishes this state from a fresh instance (see
  // isChainAtDefault); the top bar's New button greys out on it.
  state->setProperty("atDefault", atDefault);
  if (presetId.isNotEmpty()) {
    juce::DynamicObject::Ptr preset = new juce::DynamicObject();
    preset->setProperty("id", presetId);
    preset->setProperty("name", presetName);
    state->setProperty("preset", juce::var(preset.get()));
  }
  state->setProperty("stereoEnabled", stereo);
  // The scene the blocks above are in, "A" to "H".
  state->setProperty("activeScene", sceneLetter(activeScene));
  // Active branch (stereo mode only): which lane is the trunk and which of
  // its tone blocks feeds the other lane. Absent when the chains are
  // independent, and while mono, where a set branch lies dormant.
  if (branched) {
    juce::DynamicObject::Ptr branch = new juce::DynamicObject();
    branch->setProperty("side", branchSide);
    branch->setProperty("afterBlockId", branchAfter);
    state->setProperty("branch", juce::var(branch.get()));
  }
  state->setProperty("activeSide", activeSide);
  // True when a real stereo source feeds the plugin (stereo host bus or a
  // stereo standalone input device); drives the faceplate input-mode button
  // and the dual input meters.
  state->setProperty("stereoInput", stereoInputDetected.load());
  // Output-side twin: false on a mono rig (mono host bus, or a one-channel
  // standalone output device). Spread is idle then (the UI greys it out),
  // and stereo chains are summed to mono (see processImageStage): the UI
  // dims the pans and shows the MONO chip on the pan rail.
  state->setProperty("stereoOutput", stereoOutputDetected.load());
  // True in the standalone app; gates standalone-only settings.
  state->setProperty("standalone", isStandalone());
  // In grid mode the rows name their own inputs: the mode a legacy rig
  // would have when the grid is one, else "grid".
  state->setProperty("inputMode", gridMode ? projectedInputMode
                                           : inputModeToString(getInputMode()));
  // True while dual mono is actually running (mode selected on a mono chain
  // with a stereo source *and* a stereo rig; see dualMonoEngaged). The UI
  // shows stereo output meters and the Balance control, and dims Spread.
  state->setProperty("dualMonoActive", gridMode ? projectedDualMono : dualMonoEngaged());
  // Machine-wide settings ride this payload because they change together
  // with a revision bump, like everything else Settings displays.
  state->setProperty("namSlimSizeDefault", namSlimSizeDefault.load());
  state->setProperty("multiCore", multiCoreEnabled.load());
  // The EQ editor mirrors the biquad math client-side; block EQs run in the
  // chain domain, so the drawn curve must use the live chain rate (48 kHz x
  // oversampling factor), not the host rate (see ChainDomain.h).
  state->setProperty("sampleRate", chainSampleRate());
  return state.get();
}

juce::var TONE3000Processor::getMeterLevels() const {
  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  // Main meters ship as [L, R] pairs (mono sources report L == R). The UI
  // store derives the combined mono value as max(L, R).
  auto channelPair = [](float l, float r) {
    juce::Array<juce::var> pair;
    pair.add(l);
    pair.add(r);
    return juce::var(pair);
  };
  root->setProperty("input", channelPair(inputMeterLevelL.load(), inputMeterLevelR.load()));
  root->setProperty("output", channelPair(outputMeterLevelL.load(), outputMeterLevelR.load()));
  // Audio-callback load as a 0..1 proportion (the hint bar shows it as a %).
  root->setProperty("cpu", loadMeasurer.getLoadAsProportion());
  // Stereo-image output correlation (-1..1, 1 when the engine is idle) for
  // the mono-compatibility meter: whichever image engine the mode runs
  // (Spread in mono, the Align deck in stereo). Riding this poll costs no
  // extra call.
  root->setProperty("correlation", stereoEnabled.load() ? stereoOffset.correlation()
                                                        : spread.correlation());

  // Everything the chain owns is read into plain values under the lock, in
  // one hold, with no allocation beyond the reserved vectors; the var
  // objects (allocations the audio thread's try-lock would otherwise wait
  // behind) are built after it is released.
  struct BlockMeter {
    std::string id;
    float in{0.0f}, out{0.0f};
    size_t firstReading{0}, numReadings{0};
  };
  struct ReadingValue {
    juce::String id;
    float value{0.0f};
  };
  struct RowMeter {
    bool active{false};
    float in[2]{-60.0f, -60.0f}, out[2]{-60.0f, -60.0f};
  };
  struct SplitMeter {
    std::string id;
    int row{0};
    bool active{false};
    float split[2]{-60.0f, -60.0f}, mix[2]{-60.0f, -60.0f};
  };
  struct OutputMeter {
    int pair{0};
    bool main{false};
    float level[2]{0.0f, 0.0f};
  };
  std::vector<BlockMeter> blockMeters;
  std::vector<ReadingValue> readingValues;
  std::vector<RowMeter> rowMeters;
  std::vector<SplitMeter> splitMeters;
  std::vector<OutputMeter> outputMeters;
  blockMeters.reserve(64);
  readingValues.reserve(256);
  rowMeters.reserve(static_cast<size_t>(kGridRows));
  splitMeters.reserve(16);
  outputMeters.reserve(static_cast<size_t>(kMaxIoPairs));
  bool gridMode = false;
  {
    juce::ScopedLock lock(chainMutex);
    for (const auto& chain : lanes) {
      for (const auto& block : chain) {
        if (block->type == ChainBlockType::INSERT)
          continue;
        BlockMeter meter;
        meter.id = block->id;
        meter.in = block->inputMeterDb.load();
        meter.out = block->outputMeterDb.load();
        meter.firstReading = readingValues.size();
        // The processor is only swapped under chainMutex, held here.
        if (block->processor != nullptr) {
          const BlockReadingTable& table = block->processor->readingTable();
          for (size_t i = 0; i < table.size(); ++i)
            readingValues.push_back(
                {table[i].id, block->processor->getReading(static_cast<int>(i))});
          meter.numReadings = readingValues.size() - meter.firstReading;
        }
        blockMeters.push_back(std::move(meter));
      }
    }

    // The grid's own meters: per row what entered it and what it sent, per
    // output pair the level after its strip, and the plan CPU value. A
    // legacy rig has no rows; its one output is the main pair.
    gridMode = routingMode == RoutingMode::Grid;
    if (gridMode) {
      for (int row = 0; row < kGridRows; ++row) {
        const GridRow& gridRow = grid.rows[static_cast<size_t>(row)];
        const ProcessingPlan::RowInfo& info = activePlan.rowInfo[static_cast<size_t>(row)];
        const bool active = info.active;
        RowMeter meter;
        meter.active = active;
        // What entered the row: from its strip or the rows that feed it,
        // or for the lower row of a split what the splitter sent down. What
        // it sent: to its bus or the row it feeds; a row that ends in a
        // mixer has the mixer's own meter instead.
        const SplitJunction* above = row > 0 ? splitOfRow(row - 1) : nullptr;
        if (active && info.source == ProcessingPlan::RowInfo::FromSplit && above != nullptr) {
          meter.in[0] = meter.in[1] = above->splitMeterDb(1);
        } else if (active) {
          meter.in[0] = gridRow.meterIn[0].load();
          meter.in[1] = gridRow.meterIn[1].load();
        }
        if (active && info.sink == ProcessingPlan::RowInfo::ToMix && above != nullptr) {
          meter.out[0] = meter.out[1] = above->mixMeterDb(1);
        } else if (active) {
          meter.out[0] = gridRow.meterOut[0].load();
          meter.out[1] = gridRow.meterOut[1].load();
        }
        rowMeters.push_back(meter);
      }
      // Per split: what the splitter sent down each path (A stays in the
      // upper row, B goes to the lower one) and what reached the mixer from
      // each, after alignment and before the mixer's levels.
      for (const auto& junction : grid.splits) {
        const bool active =
            activePlan.rowInfo[static_cast<size_t>(juce::jlimit(0, kGridRows - 1, junction->upperRow))]
                .active;
        SplitMeter meter;
        meter.id = junction->id;
        meter.row = junction->upperRow;
        meter.active = active;
        if (active) {
          meter.split[0] = junction->splitMeterDb(0);
          meter.split[1] = junction->splitMeterDb(1);
          if (junction->hasRejoin()) {
            meter.mix[0] = junction->mixMeterDb(0);
            meter.mix[1] = junction->mixMeterDb(1);
          }
        }
        splitMeters.push_back(std::move(meter));
      }
    }
    for (int b = 0; b < activePlan.numBuses && b < kMaxIoPairs; ++b) {
      const ProcessingPlan::Bus& bus = activePlan.buses[static_cast<size_t>(b)];
      OutputMeter meter;
      meter.pair = static_cast<int>(bus.pair) + 1;
      meter.main = bus.mainStrip;
      meter.level[0] = bus.mainStrip ? outputMeterLevelL.load()
                                     : outputBuses[static_cast<size_t>(b)].meterL.load();
      meter.level[1] = bus.mainStrip ? outputMeterLevelR.load()
                                     : outputBuses[static_cast<size_t>(b)].meterR.load();
      outputMeters.push_back(meter);
    }
  }

  juce::DynamicObject::Ptr blocks = new juce::DynamicObject();
  for (const auto& meter : blockMeters) {
    juce::DynamicObject::Ptr levels = new juce::DynamicObject();
    levels->setProperty("in", meter.in);
    levels->setProperty("out", meter.out);
    if (meter.numReadings > 0) {
      juce::DynamicObject::Ptr readings = new juce::DynamicObject();
      for (size_t i = 0; i < meter.numReadings; ++i) {
        const ReadingValue& reading = readingValues[meter.firstReading + i];
        readings->setProperty(reading.id, reading.value);
      }
      levels->setProperty("readings", juce::var(readings.get()));
    }
    blocks->setProperty(juce::String(meter.id), juce::var(levels.get()));
  }
  root->setProperty("blocks", juce::var(blocks.get()));

  juce::Array<juce::var> rows, outputs, splits;
  if (gridMode) {
    int rowIndex = 0;
    for (const auto& meter : rowMeters) {
      juce::DynamicObject::Ptr levels = new juce::DynamicObject();
      levels->setProperty("row", rowIndex++);
      levels->setProperty("active", meter.active);
      levels->setProperty("in", channelPair(meter.in[0], meter.in[1]));
      levels->setProperty("out", channelPair(meter.out[0], meter.out[1]));
      rows.add(juce::var(levels.get()));
    }
    for (const auto& meter : splitMeters) {
      juce::DynamicObject::Ptr levels = new juce::DynamicObject();
      levels->setProperty("id", juce::String(meter.id));
      levels->setProperty("row", meter.row);
      levels->setProperty("active", meter.active);
      levels->setProperty("split", channelPair(meter.split[0], meter.split[1]));
      levels->setProperty("mix", channelPair(meter.mix[0], meter.mix[1]));
      splits.add(juce::var(levels.get()));
    }
  }
  for (const auto& meter : outputMeters) {
    juce::DynamicObject::Ptr levels = new juce::DynamicObject();
    levels->setProperty("pair", meter.pair);
    levels->setProperty("main", meter.main);
    levels->setProperty("level", channelPair(meter.level[0], meter.level[1]));
    outputs.add(juce::var(levels.get()));
  }
  root->setProperty("rows", rows);
  root->setProperty("splits", splits);
  root->setProperty("outputs", outputs);
  root->setProperty("planCpu", getPlanCpu());
  return root.get();
}

// ####################
// STEREO MODE
// ####################
void TONE3000Processor::setStereoMode(bool enabled) {
  // Mono ↔ stereo rewires the whole routing (one chain on both channels ↔
  // two independent lanes) with no single block to fade, so mute-splice like
  // reorder. Cheap early-out first: no fade when nothing changes.
  // A legacy notion: a grid goes back to the legacy rig first when it can
  // (its own undo step), and is left alone when it cannot.
  if (isGridMode() && !static_cast<bool>(convertRouting(false)["ok"]))
    return;

  if (stereoEnabled.load() == enabled)
    return;

  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (stereoEnabled.load() == enabled)
    return;

  pushChainHistory();

  stereoEnabled.store(enabled);

  if (!enabled)
    pendingAddSide = ChainSide::Left;

  // Branching only runs between two chains: dormant while mono (the fields
  // persist so toggling back re-engages the branch), live again in stereo.
  // Alignment also seeds the right chain's minimum slot layout the first
  // time stereo is enabled (legacy states that only carried one insert get
  // padded here too), and a re-engaging branch re-evens the lane ends
  // (mono edits may have changed the trunk's length underneath it).
  rebuildTopology();

  // A re-engaged branch means a single mono source again, so re-enforce the
  // input-mode invariant (the feed may have gone back to stereo / dual mono
  // while the branch lay dormant).
  if (rtBranchTapIndex >= 0 && isStereoFeed(getInputMode()))
    inputMode.store(static_cast<int>(InputMode::Left));

  // Make sure the right chain's engines are ready to run in the chain domain.
  if (enabled)
    prepareChain(laneIndex(ChainSide::Right));

  // Dual mono only applies to a mono chain, so with it selected the mode
  // switch moves the NAM voice requirement (see wantedNamVoices): rebuild
  // the engines and hold this fade until they land, like a preset load.
  if (requeueNamEnginesForVoiceCount())
    editFade.releaseWhenChainLoadsSettle();

  bumpChainRevision();
  DBG("Stereo mode " << (enabled ? "enabled" : "disabled"));
}

// ####################
// CHAIN BRANCHING
// ####################

// Re-resolve branchAfterBlockId into rtBranchTapIndex for the RT path.
// Clears the branch entirely when the tapped block is no longer a tone block
// in the trunk lane (removed, moved across, stale snapshot). Validated in
// mono mode too, so edits made while the branch is dormant can't leave a
// stale id behind. Called after every structural change; callers own
// history/revision/fade; this is pure bookkeeping.
void TONE3000Processor::refreshBranchTapIndex() {
  rtBranchTapIndex = -1;
  if (branchAfterBlockId.empty())
    return;

  const auto& trunk = lane(branchSourceSide);
  int tapIdx = -1;
  for (int i = 0; i < static_cast<int>(trunk.size()); ++i) {
    const auto& b = trunk[static_cast<size_t>(i)];
    if (b != nullptr && b->id == branchAfterBlockId && b->type != ChainBlockType::INSERT) {
      tapIdx = i;
      break;
    }
  }
  if (tapIdx == -1) {
    DBG("Branch tap block left the trunk lane; reverting to independent chains");
    branchAfterBlockId.clear();
    return;
  }

  // Dormant in mono mode: the fields persist (a mono round trip brings the
  // branch back) but the RT path ignores them (like the right lane itself).
  if (stereoEnabled.load())
    rtBranchTapIndex = tapIdx;
}

// Keep the lanes' visible ends even while a branch is active. The branch
// lane renders indented past the trunk's tap gap (its input is that trunk
// prefix's output), so with both lanes at the per-lane baseline its rail
// overshoots the trunk's end by the whole indent, all trailing empty
// placeholders. Those are free real estate, so trim them (never below the
// one insert every lane keeps) until both lanes end on the same slot
// column. Trim-only, best-effort: no lane ever grows extra placeholders
// past the per-lane baseline, tone blocks never move, so lanes that
// genuinely need to be uneven (a long trunk, or branch tones running past
// the trunk's end) simply stay uneven. Without an active branch this
// restores the plain per-lane baseline. Pure bookkeeping like
// normalizeLaneInserts: callers own history/revision/fade.
//
// This is the topology funnel (see the declaration): after the lanes have
// their final shape it compiles the processing plan for them and runs the
// latency pass, so the plan the audio thread picks up next always describes
// the lanes it will run.
void TONE3000Processor::rebuildTopology() {
  // Baseline first (also undoes earlier alignment, so branch moves never
  // compound), then re-resolve the tap: insert churn can shift its index.
  // Legacy rows only: the insert-slot invariant is a property of the two
  // legacy lanes, and rows 2 and 3 hold nothing in this mode.
  if (routingMode == RoutingMode::Legacy)
    for (int row = 0; row < kNumLegacyLanes; ++row)
      normalizeLaneInserts(lanes[static_cast<size_t>(row)]);
  else
    padGridRows();  // a grid row is exactly eight slots
  refreshBranchTapIndex();

  if (rtBranchTapIndex >= 0) {
    const Lane& trunk = lane(branchSourceSide);
    Lane& branchLane =
        lane(branchSourceSide == ChainSide::Left ? ChainSide::Right : ChainSide::Left);

    // The branch lane's first tile sits `indent` slot columns into the trunk.
    const int indent = rtBranchTapIndex + 1;
    const int targetSlots = static_cast<int>(trunk.size()) - indent;

    int inserts =
        static_cast<int>(std::count_if(branchLane.begin(), branchLane.end(), isInsertBlock));
    while (static_cast<int>(branchLane.size()) > targetSlots && inserts > 1 &&
           isInsertBlock(branchLane.back())) {
      branchLane.pop_back();
      --inserts;
    }
  }

  // Every structural change comes through here, which makes it the place to
  // re-derive what depends on which blocks sit where: the plan (which rows
  // run, over which block ranges, forked where) and the latency
  // compensation, which is the plan's latency pass. Both after the trim
  // above, so the plan's ranges are the lanes' final lengths (the trim
  // itself moves no latency: placeholders have none).
  compileActivePlan();
  refreshLatencyCompensation();
  // A plan with more or fewer strips or buses needs the boundary reopened
  // for them, here: off the audio thread, under the lock it runs with.
  syncPlanPorts();
}

bool TONE3000Processor::setChainBranch(const juce::String& side,
                                       const std::string& afterBlockId) {
  if (isGridMode() && !static_cast<bool>(convertRouting(false)["ok"]))
    return false;  // see setStereoMode

  // Rerouting one whole lane's input is structural (no single block to
  // fade), so mute-splice like reorder.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (!stereoEnabled.load()) {
    DBG("setChainBranch: only valid in stereo mode");
    return false;
  }

  const ChainSide trunkSide = side == "right" ? ChainSide::Right : ChainSide::Left;
  const auto& trunk = lane(trunkSide);
  const bool tapExists =
      std::any_of(trunk.begin(), trunk.end(), [&](const std::unique_ptr<ChainBlock>& b) {
        return b != nullptr && b->id == afterBlockId && b->type != ChainBlockType::INSERT;
      });
  if (!tapExists) {
    DBG("setChainBranch: tap block not a tone block in the " << side << " lane: "
                                                             << afterBlockId);
    return false;
  }

  // Re-pointing to the spot already tapped is a no-op: no history entry,
  // no revision bump (the UI can re-fire on fast clicks).
  if (branchSourceSide == trunkSide && branchAfterBlockId == afterBlockId)
    return true;

  pushChainHistory();

  branchSourceSide = trunkSide;
  branchAfterBlockId = afterBlockId;
  // The branch lane just gained an indent past the tap gap; its surplus
  // trailing insert placeholders trim away to end level with the trunk.
  rebuildTopology();

  // A branched chain has a single (mono) source: the trunk's channel. A
  // stereo feed would silently drop the other channel, so force a definite
  // pick; the UI hides the "stereo" / "dual mono" options while branched.
  // (Branching is stereo-mode only, where the NAM voice requirement is
  // already one, so this never moves it.)
  if (isStereoFeed(getInputMode()))
    inputMode.store(static_cast<int>(InputMode::Left));

  bumpChainRevision();
  DBG("Chain branch set: " << side << " after block " << afterBlockId);
  return true;
}

bool TONE3000Processor::clearChainBranch() {
  // A grid that is a legacy rig in shape goes back to being one first (see
  // setStereoMode); its branch, when it has one, is then cleared as ever.
  if (!leaveGridForLaneEdit())
    return false;

  {
    juce::ScopedLock lock(chainMutex);
    if (branchAfterBlockId.empty())
      return false;  // nothing to clear: no fade, no history entry
  }

  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);
  if (branchAfterBlockId.empty())
    return false;

  pushChainHistory();
  branchAfterBlockId.clear();
  // Independent lanes go back to the plain per-lane baseline (any trailing
  // inserts trimmed for the branch grow back).
  rebuildTopology();
  bumpChainRevision();
  DBG("Chain branch cleared; chains independent again");
  return true;
}

void TONE3000Processor::setActiveEditChain(const juce::String& side) {
  juce::ScopedLock lock(chainMutex);
  if (side == "right")
    pendingAddSide = ChainSide::Right;
  else if (side == "left")
    pendingAddSide = ChainSide::Left;
  bumpChainRevision();
}

bool TONE3000Processor::swapChains() {
  if (isGridMode() && !static_cast<bool>(convertRouting(false)["ok"]))
    return false;  // see setStereoMode

  // Both lanes change output channel at once; mute-splice like reorder.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  if (!stereoEnabled.load())
    return false;

  pushChainHistory();
  // Insert slots travel with their lane (ids are lane-agnostic UUIDs, so
  // global uniqueness is preserved); each lane's slot invariant moves
  // wholesale with its blocks.
  std::swap(lane(ChainSide::Left), lane(ChainSide::Right));

  // The trunk lane moved sides; the branch (and the lane-end alignment its
  // geometry drives) moves with it.
  branchSourceSide =
      branchSourceSide == ChainSide::Left ? ChainSide::Right : ChainSide::Left;
  rebuildTopology();

  // Polarity flips describe the captures, so they travel with their lanes
  // (pans stay put: they're image placement, not chain state).
  auto* invLeft = parameters.getParameter("chainInvertLeft");
  auto* invRight = parameters.getParameter("chainInvertRight");
  if (invLeft != nullptr && invRight != nullptr && invLeft->getValue() != invRight->getValue()) {
    const float left = invLeft->getValue();
    invLeft->setValueNotifyingHost(invRight->getValue());
    invRight->setValueNotifyingHost(left);
  }

  bumpChainRevision();
  DBG("Swapped Left/Right chains");
  return true;
}

bool TONE3000Processor::setBlockReleased(const std::string& blockId, bool released) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  // The audio thread reads it on its next callback and glides from there
  // (processChainOnBuffer), so there is nothing to splice here.
  block->released.store(released);
  return true;
}

bool TONE3000Processor::isBlockReleased(const std::string& blockId) const {
  juce::ScopedLock lock(chainMutex);
  for (const auto& l : lanes)
    for (const auto& block : l)
      if (block->id == blockId)
        return block->released.load();
  return false;
}

bool TONE3000Processor::setBlockParam(const std::string& blockId, const juce::String& param,
                                      double value) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;
  // A missing-kind placeholder is written back exactly as it was loaded, so
  // an edit could not be kept.
  if (block->isMissingKind())
    return false;

  // Validate before recording history, so failed calls never leave an entry.
  const bool isContinuous = param == "inputGain" || param == "outputGain" || param == "mix";
  const bool isKnown = isContinuous || param == "enabled" || param == "normalize";
  if (!isKnown) {
    // Not a common parameter: one of the block's kind, in its real unit.
    const int index = block->processor != nullptr ? block->processor->findParameter(param) : -1;
    if (index < 0) {
      DBG("setBlockParam: unknown param: " << param);
      return false;
    }
    const BlockParamSpec& spec = block->processor->parameterTable()[static_cast<size_t>(index)];
    const bool continuous = spec.type == BlockParamType::Float;
    pushChainHistory(continuous ? "param:" + juce::String(blockId) + ":" + param
                                : juce::String());
    // A lock-free store; the processor picks it up on its next block and
    // smooths what needs smoothing.
    autoAssignSceneParam(*block, param);
    block->processor->setParameter(index, static_cast<float>(value));
    // A scene-assigned parameter: this is the active scene's value (the
    // other scenes keep theirs).
    if (const auto assigned = scenes.assigned.find({blockId, param});
        assigned != scenes.assigned.end())
      assigned->second[static_cast<size_t>(scenes.active)] =
          block->processor->getParameter(index);

    // The tail and the latency are read off the processor, so a parameter
    // that moves either leaves the reported values stale. Refreshed once the
    // gesture settles, like the revision: a knob drag would otherwise
    // re-report (and, for latency, mute-splice) on every tick.
    if (spec.affectsTail || spec.affectsLatency) {
      blockRefreshPending.store(true);
      startTimer(static_cast<int>(kGestureSettleMs));
    }

    if (continuous)
      deferredRevisionBump();
    else
      bumpChainRevision();
    return true;
  }

  // Continuous params coalesce a whole knob drag into one undo step.
  pushChainHistory(isContinuous ? "param:" + juce::String(blockId) + ":" + param
                                : juce::String());
  autoAssignSceneParam(*block, param);

  if (param == "enabled") {
    // The switch belongs to the active scene. Before its first move the
    // block was the same in every scene (see SceneStore); it gets a state of
    // its own in each, and this edit changes one.
    registerSceneBlock(*block);
    block->enabled = value > 0.5;
    scenes.blockStates[block->id][static_cast<size_t>(scenes.active)] = block->enabled;
  } else if (param == "normalize") {
    // The toggle alone: pushing the whole state would re-assert the A2 size
    // on the engine as well.
    block->processorState.normalize = value > 0.5;
    if (NamBlockProcessor* nam = block->nam())
      nam->setNormalizeEnabled(block->processorState.normalize);
  } else if (param == "inputGain") {
    block->inputGainNormalized = juce::jlimit(0.0f, 1.0f, static_cast<float>(value));
  } else if (param == "outputGain") {
    block->outputGainNormalized = juce::jlimit(0.0f, 1.0f, static_cast<float>(value));
  } else if (param == "mix") {
    block->mixNormalized = juce::jlimit(0.0f, 1.0f, static_cast<float>(value));
  }

  // Scene-assigned: the active scene's value, as for a kind parameter.
  if (isContinuous) {
    if (const auto assigned = scenes.assigned.find({blockId, param});
        assigned != scenes.assigned.end())
      readSceneParam(*block, param, assigned->second[static_cast<size_t>(scenes.active)]);
  }

  // Continuous drags settle into one bump after the gesture ends; discrete
  // toggles resync immediately.
  if (isContinuous)
    deferredRevisionBump();
  else
    bumpChainRevision();
  return true;
}

bool TONE3000Processor::setBlockDescription(const std::string& blockId, const juce::String& text,
                                            juce::String& error) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::KIND || block->processor == nullptr ||
      block->isMissingKind()) {
    error = "the block is not a block of a kind that takes a description";
    return false;
  }
  if (!block->processor->takesDescription()) {
    error = "a '" + block->kindId + "' block takes no description";
    return false;
  }
  // Validate before recording history, so a refused file never leaves an entry.
  if (!block->processor->checkDescription(text, error))
    return false;
  pushChainHistory();
  // The chain lock keeps the audio thread out while the kind rebuilds.
  if (!block->processor->loadDescription(text, error)) {
    return false;
  }
  if (const juce::String own = block->processor->displayName(); own.isNotEmpty())
    block->kindName = own;
  bumpChainRevision();
  return true;
}

juce::var TONE3000Processor::getBlockParams(const std::string& blockId) const {
  // A hosted plugin's parameter text is a call into the plugin (message
  // thread, and the VST3 wrapper may take the message-manager lock), so it
  // is read after the chain lock is released, through the instance's
  // shared_ptr: the block can be removed meanwhile, the instance cannot die.
  std::shared_ptr<HostedPluginInstance> hostedInstance;
  juce::Array<juce::var> params;
  {
    juce::ScopedLock lock(chainMutex);
    const ChainBlock* block = nullptr;
    for (const auto& l : lanes)
      for (const auto& b : l)
        if (b->id == blockId)
          block = b.get();
    if (block == nullptr || block->type == ChainBlockType::INSERT)
      return {};

    const auto add = [&params](const BlockParamSpec& spec, const char* scope, double value) {
      juce::var described = spec.toVar();
      described.getDynamicObject()->setProperty("scope", scope);
      described.getDynamicObject()->setProperty("value", value);
      params.add(described);
    };

    for (const auto& spec : commonBlockParams()) {
      double value = 0.0;
      if (spec.id == "enabled")
        value = block->enabled ? 1.0 : 0.0;
      else if (spec.id == "normalize")
        value = block->processorState.normalize ? 1.0 : 0.0;
      else if (spec.id == "inputGain")
        value = block->inputGainNormalized;
      else if (spec.id == "outputGain")
        value = block->outputGainNormalized;
      else if (spec.id == "mix")
        value = block->mixNormalized;
      add(spec, "common", value);
    }

    if (const auto* hosted = HostedPluginBlockProcessor::from(block->processor.get())) {
      hostedInstance = hosted->instance();
    } else if (block->processor != nullptr) {
      const BlockParamTable& table = block->processor->parameterTable();
      for (size_t i = 0; i < table.size(); ++i)
        add(table[i], "kind",
            static_cast<double>(block->processor->getParameter(static_cast<int>(i))));
    }
  }

  if (hostedInstance != nullptr) {
    const BlockParamTable& table = hostedInstance->parameterTable();
    for (size_t i = 0; i < table.size(); ++i) {
      juce::var described = table[i].toVar();
      described.getDynamicObject()->setProperty("scope", "kind");
      described.getDynamicObject()->setProperty(
          "value", static_cast<double>(hostedInstance->getParameter(static_cast<int>(i))));
      // What the value means: the plugin's 0..1 is not a real unit.
      const juce::String text = hostedInstance->parameterText(static_cast<int>(i));
      if (text.isNotEmpty())
        described.getDynamicObject()->setProperty("text", text);
      params.add(described);
    }
  }
  return params;
}

bool TONE3000Processor::setBlockSlimSize(const std::string& blockId, double slimSize) {
  slimSize = juce::jlimit(0.0, 1.0, slimSize);

  // Cheap early-out first: no fade (and no history entry) when nothing
  // changes.
  {
    juce::ScopedLock lock(chainMutex);
    const ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || block->type != ChainBlockType::NAM) {
      DBG("setBlockSlimSize: not a NAM block: " << blockId);
      return false;
    }
    if (block->processorState.slimSize == slimSize)
      return true;
  }

  // Retiering swaps the running engine's weights discontinuously and can
  // prewarm the new submodel while holding chainMutex, so mute-splice like
  // a structural edit; the fade keeps the audio thread off the lock (see
  // processBlock's try-lock) while the swap runs.
  ChainEditFade editFade(*this);
  juce::ScopedLock lock(chainMutex);

  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type != ChainBlockType::NAM)
    return false;
  if (block->processorState.slimSize == slimSize)
    return true;

  pushChainHistory();
  block->processorState.slimSize = slimSize;
  // A still-loading block only records the size here: the in-flight prepare
  // read the old value, and applyPreparedModelToChainBlock re-asserts the
  // block's size when that engine lands.
  block->pushProcessorState();

  bumpChainRevision();
  return true;
}

bool TONE3000Processor::toggleBlockPower(int position, bool rightLane) {
  // The Right lane only processes in stereo mode; a mapped right-block stomp
  // outside it must not edit chain state the user can't see.
  if (rightLane && !isStereoMode())
    return false;

  // Resolve the position to a block id under the lock, then route through
  // setBlockParam so a MIDI stomp is exactly a UI power click: undoable,
  // revision-bumped, same validation.
  std::string blockId;
  bool enabled = false;
  {
    juce::ScopedLock lock(chainMutex);
    int seen = 0;
    for (const auto& block : lane(rightLane ? ChainSide::Right : ChainSide::Left)) {
      if (block->type == ChainBlockType::INSERT)
        continue;
      if (seen++ == position) {
        blockId = block->id;
        enabled = block->enabled;
        break;
      }
    }
  }
  if (blockId.empty())
    return false;  // chain shorter than the mapped slot
  return setBlockParam(blockId, "enabled", enabled ? 0.0 : 1.0);
}

// ####################
// PER-BLOCK EQ
// ####################
bool TONE3000Processor::setBlockEqBand(const std::string& blockId, int bandIndex,
                                       const juce::var& bandVar) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  // (A missing-kind placeholder is saved back as it was loaded, so an EQ
  // edit on it could not be kept: refused, like setBlockParam.)
  if (block == nullptr || block->type == ChainBlockType::INSERT || block->isMissingKind())
    return false;
  if (!bandVar.isObject() || bandIndex < 0 || bandIndex >= BlockEq::kNumBands)
    return false;

  // A whole dot/slider drag coalesces into one undo step.
  pushChainHistory("eq:" + juce::String(blockId) + ":" + juce::String(bandIndex));

  if (!block->eq.setBandFromVar(bandIndex, bandVar))
    return false;

  // Band edits arrive at drag rate; converge pollers after the gesture ends.
  deferredRevisionBump();
  return true;
}

bool TONE3000Processor::setBlockEqEnabled(const std::string& blockId, bool enabled) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  // (A missing-kind placeholder is saved back as it was loaded, so an EQ
  // edit on it could not be kept: refused, like setBlockParam.)
  if (block == nullptr || block->type == ChainBlockType::INSERT || block->isMissingKind())
    return false;
  if (block->eq.isEnabled() == enabled)
    return true;

  pushChainHistory();
  block->eq.setEnabled(enabled);
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::setBlockEqPre(const std::string& blockId, bool pre) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  // (A missing-kind placeholder is saved back as it was loaded, so an EQ
  // edit on it could not be kept: refused, like setBlockParam.)
  if (block == nullptr || block->type == ChainBlockType::INSERT || block->isMissingKind())
    return false;
  if (block->eq.isPre() == pre)
    return true;

  pushChainHistory();
  block->eq.setPre(pre);
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::resetBlockEq(const std::string& blockId) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  // (A missing-kind placeholder is saved back as it was loaded, so an EQ
  // edit on it could not be kept: refused, like setBlockParam.)
  if (block == nullptr || block->type == ChainBlockType::INSERT || block->isMissingKind())
    return false;

  pushChainHistory();
  block->eq.resetToDefault();
  bumpChainRevision();
  return true;
}

bool TONE3000Processor::setBlockSpectrumEnabled(const std::string& blockId, bool enabled) {
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return false;

  block->spectrum.setEnabled(enabled);
  return true;
}

juce::var TONE3000Processor::getBlockSpectrum(const std::string& blockId) {
  // getSpectrum does the FFT on this (message) thread over a lock-free ring,
  // so the chain lock is only held for the block lookup + analysis.
  juce::ScopedLock lock(chainMutex);
  ChainBlock* block = findBlockById(blockId);
  if (block == nullptr || block->type == ChainBlockType::INSERT)
    return {};

  return block->spectrum.getSpectrum();
}

void TONE3000Processor::disableAllBlockSpectrums() {
  juce::ScopedLock lock(chainMutex);
  for (auto& chain : lanes)
    for (auto& block : chain)
      block->spectrum.setEnabled(false);
}

void TONE3000Processor::setAccessToken(const juce::String& token) {
  juce::ScopedLock lock(accessTokenMutex);
  accessToken = token;
  DBG("TONE3000 access token updated (" << token.length() << " chars)");
}

juce::String TONE3000Processor::getAccessToken() const {
  juce::ScopedLock lock(accessTokenMutex);
  return accessToken;
}
