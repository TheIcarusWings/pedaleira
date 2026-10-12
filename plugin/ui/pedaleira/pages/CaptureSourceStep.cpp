#include "../Icons.h"
#include "../Paint.h"
#include "../Type.h"
#include "CaptureViews.h"

namespace pedaleira::ui {

namespace {

constexpr int kCardHeight = 300;
constexpr int kCardGap = 24;
constexpr int kDetailsHeight = 132;

const char* const kSourceIds[] = {"pedal", "ampHead", "ampCab", "modeller"};
const char* const kGainIds[] = {"clean", "crunch", "highGain"};
const char* const kSizeIds[] = {"full", "lite"};

int indexOf(const char* const* ids, int count, const juce::String& id, int fallback) {
  for (int i = 0; i < count; ++i)
    if (id == ids[i])
      return i;
  return fallback;
}

ChoiceCard::Spec cardSpec(int index) {
  ChoiceCard::Spec spec;
  switch (index) {
    case 0:
      spec.icon = &icons::path(icons::Icon::capturePedal);
      spec.name = "Pedal";
      spec.description = "Direct at line level, through a re-amp box.";
      spec.needs = {"Re-amp box", "Two instrument cables", "TRS cable to the interface"};
      break;
    case 1:
      spec.icon = &icons::path(icons::Icon::captureAmpHead);
      spec.name = "Amp Head";
      spec.description = "Speaker out into a load box, DI out to the interface.";
      spec.needs = {"Load box", "Re-amp box", "Speaker cable"};
      break;
    case 2:
      spec.icon = &icons::path(icons::Icon::captureAmpCab);
      spec.name = "Amp + Cab";
      spec.description = "A microphone on the speaker, in a quiet room.";
      spec.needs = {"Re-amp box", "Microphone", "Mic stand and XLR cable"};
      break;
    default:
      spec.icon = &icons::path(icons::Icon::captureModeller);
      spec.name = "Another modeller";
      spec.description = "Its line output straight into the interface.";
      spec.note = "Captures of commercial devices are for personal use only";
      spec.needs = {"Re-amp box", "Line output cable"};
      break;
  }
  return spec;
}

}  // namespace

CaptureSourceStep::CaptureSourceStep(CaptureHost& hostPage) : CaptureStep(hostPage) {
  setTitle("Source");
  for (int i = 0; i < 4; ++i) {
    const ChoiceCard::Spec spec = cardSpec(i);
    cards[static_cast<size_t>(i)] = std::make_unique<ChoiceCard>(spec.name, spec);
    cards[static_cast<size_t>(i)]->onClick = [this, i] { chooseSource(i); };
    addAndMakeVisible(*cards[static_cast<size_t>(i)]);
  }
  parts::styleEditor(name);
  name.setTitle("Capture name");
  name.setTextToShowWhenEmpty("Brit 800 Crunch", colour::faint);
  name.onTextChange = [this] { host.footerChanged(); };
  addAndMakeVisible(name);
  gainTag.setTitle("Gain tag");
  gainTag.setSelected(1);
  modelSize.setTitle("Model size");
  addAndMakeVisible(gainTag);
  addAndMakeVisible(modelSize);
  chooseSource(1);
}

void CaptureSourceStep::chooseSource(int index) {
  sourceIndex = juce::jlimit(0, 3, index);
  for (int i = 0; i < 4; ++i)
    cards[static_cast<size_t>(i)]->setChosen(i == sourceIndex);
}

void CaptureSourceStep::show(const juce::var& newSession) {
  session = newSession;
  if (id().isEmpty()) {
    // A fresh capture: the board's defaults.
    chooseSource(1);
    name.setText({}, false);
    gainTag.setSelected(1);
    modelSize.setSelected(0);
    return;
  }
  chooseSource(indexOf(kSourceIds, 4, session["source"].toString(), 1));
  if (name.getText() != session["name"].toString())
    name.setText(session["name"].toString(), false);
  gainTag.setSelected(indexOf(kGainIds, 3, session["gainTag"].toString(), 1));
  modelSize.setSelected(indexOf(kSizeIds, 2, session["modelSize"].toString(), 0));
}

CaptureStep::Footer CaptureSourceStep::footer() const {
  Footer footer;
  footer.backEnabled = false;
  footer.primary = "Continue to wiring";
  footer.primaryEnabled = name.getText().trim().isNotEmpty();
  return footer;
}

void CaptureSourceStep::primary() {
  const juce::String text = name.getText().trim();
  if (text.isEmpty())
    return;
  const juce::String source = kSourceIds[sourceIndex];
  const juce::String gain = kGainIds[juce::jlimit(0, 2, gainTag.getSelected())];
  const juce::String modelSizeId = kSizeIds[juce::jlimit(0, 1, modelSize.getSelected())];
  EngineReply reply;
  if (id().isEmpty()) {
    reply = host.run("capture.create",
                     object({{"name", text}, {"source", source}, {"gainTag", gain}, {"modelSize", modelSizeId}}),
                     "Could not start the capture");
    if (reply.ok)
      host.adopt(reply.value);
  } else {
    reply = host.run("capture.setSource",
                     object({{"id", id()}, {"name", text}, {"source", source}, {"gainTag", gain}, {"modelSize", modelSizeId}}),
                     "Could not change the capture");
  }
  if (reply.ok)
    host.moveTo(capture::Step::wiring);
}

juce::Rectangle<int> CaptureSourceStep::detailsBounds() const {
  const int top = parts::kHeadingHeight + parts::kBlockGap + kCardHeight + parts::kBlockGap;
  return {0, top, getWidth(), kDetailsHeight};
}

void CaptureSourceStep::resized() {
  const int width = (getWidth() - 3 * kCardGap) / 4;
  const int top = parts::kHeadingHeight + parts::kBlockGap;
  for (int i = 0; i < 4; ++i)
    cards[static_cast<size_t>(i)]->setBounds(i * (width + kCardGap), top, width, kCardHeight);

  // The details card: the name takes what the two choices leave.
  auto inner = detailsBounds().reduced(24, 20);
  auto sizeColumn = inner.removeFromRight(juce::jmax(modelSize.idealWidth(), 330));
  inner.removeFromRight(40);
  auto gainColumn = inner.removeFromRight(gainTag.idealWidth() + 40);
  inner.removeFromRight(40);
  const int controlTop = inner.getY() + 24;
  name.setBounds(inner.getX(), controlTop, inner.getWidth(), 40);
  gainTag.setBounds(gainColumn.getX(), controlTop + 1, gainTag.idealWidth(), 38);
  modelSize.setBounds(sizeColumn.getX(), controlTop + 1, modelSize.idealWidth(), 38);
}

void CaptureSourceStep::paint(juce::Graphics& g) {
  parts::paintHeading(g, getLocalBounds(), "What are you capturing?",
                      "Pick the kind of device. The next step shows the exact wiring for it.");
  const auto details = detailsBounds();
  parts::paintCard(g, details);
  auto inner = details.reduced(24, 20);
  auto sizeColumn = inner.removeFromRight(juce::jmax(modelSize.idealWidth(), 330));
  inner.removeFromRight(40);
  auto gainColumn = inner.removeFromRight(gainTag.idealWidth() + 40);
  inner.removeFromRight(40);
  const auto helper = [&](juce::Rectangle<int> column, const juce::String& label, const juce::String& help, int width) {
    paint::label(g, label, juce::Rectangle<int>(column.getX(), column.getY(), width, 16));
    parts::drawWrapped(g, help, type::sans(12.0f), colour::muted, juce::Rectangle<int>(column.getX(), column.getY() + 24 + 40 + 8, width, 34));
  };
  helper(inner, "Capture name", "Shown in the Library and on the block tile.", inner.getWidth());
  helper(gainColumn, "Gain tag", "Helps you pick the right capture later.", gainColumn.getWidth() - 40);
  helper(sizeColumn, "Model size", "Lite uses less CPU on a busy grid. One capture file holds both sizes.", sizeColumn.getWidth());
}

}  // namespace pedaleira::ui
