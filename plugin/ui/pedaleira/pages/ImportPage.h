// The importer (boards "Import: source", "Import: map blocks" and "Import:
// report"): five steps, Source, Choose presets, Map blocks, Import, Report.
//
// The app has no USB reader. A Quad Cortex is read by tools/import/
// quadcortex_dump.py, which writes one foreign preset file per preset; this
// page takes those files (docs/import.md). Pedaleira presets come in as JSON
// files or a folder through preset.importJson.
//
// Every import goes through preset.importForeign or preset.importJson, one
// preset at a time, so the page needs no engine code of its own. The Map
// blocks step shows the importer's own dry run (preset.previewForeign) for
// the chosen presets, and the Report is what the engine said afterwards.
#pragma once

#include "../EngineClient.h"
#include "../widgets/Controls.h"
#include "../widgets/Dialog.h"
#include "ImportModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace pedaleira::ui {

class ImportPage : public juce::Component, private juce::Timer {
public:
  explicit ImportPage(EngineClient& engine);
  ~ImportPage() override;

  enum class Step { source = 0, choose, map, run, report };

  // Reads the setlists again (the page is shown, or the library changed).
  void refresh();

  // Presets were added to the library.
  std::function<void()> onChanged;
  // A preset was loaded from the report: show the grid.
  std::function<void()> onOpenGrid;
  // The same, and open the block library on this slot for a capture.
  std::function<void(int row, int slot)> onPickCapture;

  // Testbed and tests: drive the page as a player would.
  void chooseKind(ImportKind kind);
  void setFiles(const juce::Array<juce::File>& files);
  void goTo(Step step);
  // Ticks a file of the list in or out.
  void setSelected(int index, bool selected);
  Step currentStep() const { return step; }
  // Starts the import and runs it to the end without waiting for the timer;
  // with a limit, only that many presets, and the run stays on screen.
  void runNow(int limit = -1);
  void selectOutcome(int index);
  // The text of the Markdown report, for a test.
  juce::String markdown() const;
  Dialog& dialog() { return prompt; }
  const std::vector<ImportOutcome>& results() const { return outcomes; }

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent& event) override;
  void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
  bool keyPressed(const juce::KeyPress& key) override;

private:
  enum class Area { chooseList, mapList, runList, presetList, sectionList, count };

  void timerCallback() override;

  // Flow.
  void next();
  void back();
  void reset();
  void startImport();
  void importOne();
  void finishImport();
  void chooseFiles(bool folder);
  void chooseSetlist();
  void useMappingTable();
  void saveMappingTable();
  void exportReport();
  void openOutcome(const ReportRow* pick);
  void notice(const juce::String& title, const juce::String& message);
  bool kindAvailable(int card) const;
  juce::var currentMapping() const;
  int selectedCount() const;
  int readableCount() const;
  void rebuildMapping();
  void rebuildReport();
  void buildFilter();
  void layoutButtons();
  const ImportOutcome* selectedOutcome() const;
  std::vector<int> visibleOutcomes() const;

  // Geometry.
  juce::Rectangle<int> bodyArea() const;
  juce::Rectangle<int> footerArea() const;
  juce::Rectangle<int> stepperArea() const;
  juce::Rectangle<int> sourceCardBounds(int index) const;
  juce::Rectangle<int> viewportOf(Area area) const;
  int contentHeightOf(Area area) const;
  int scrollOf(Area area) const { return scroll[static_cast<size_t>(area)]; }
  void scrollBy(Area area, int delta);

  // Painting, by step.
  void paintStepper(juce::Graphics& g);
  void paintSource(juce::Graphics& g);
  void paintChoose(juce::Graphics& g);
  void paintMap(juce::Graphics& g);
  void paintRun(juce::Graphics& g);
  void paintReport(juce::Graphics& g);
  void paintSourceCard(juce::Graphics& g, juce::Rectangle<int> area, bool withSetlist);
  void paintCarriedOver(juce::Graphics& g, juce::Rectangle<int> area);
  void paintSections(juce::Graphics& g);
  // The rows of the sections, with their positions in content space.
  struct SectionLine {
    int top = 0;
    int height = 0;
    bool heading = false;
    ReportSection::Kind kind = ReportSection::Kind::exact;
    juce::String title;
    const ReportRow* row = nullptr;
    int labelWidth = 0;
  };
  std::vector<SectionLine> sectionLines() const;
  int detailHeight(const juce::String& text, int width, bool mono) const;
  juce::String sourceName() const;
  juce::Rectangle<int> checkBounds(const SectionLine& line) const;
  juce::Rectangle<int> pickBounds(const SectionLine& line) const;

  EngineClient& engine;
  Step step = Step::source;
  ImportKind kind = ImportKind::quadCortex;
  std::vector<ImportItem> items;
  KindIndex kinds;
  std::vector<MappingRow> mapping;
  juce::File customMappingFile;
  juce::var customMapping;
  juce::String mappingProblem;

  // Where presets go; empty means a new setlist made when the import starts.
  juce::String targetSetlist;
  struct SetlistChoice {
    juce::String id, name;
    int free = 0;
  };
  std::vector<SetlistChoice> setlists;

  // The run.
  std::vector<int> queue;
  size_t cursor = 0;
  std::vector<ImportOutcome> outcomes;
  juce::String targetName;

  // The report.
  int shownOutcome = -1;
  int filter = 0;
  std::set<juce::String> checked;
  std::vector<ReportSection> sections;
  std::unique_ptr<SegmentedControl> filterControl;

  std::array<int, 5> scroll{};
  int hoverCard = -1;

  std::unique_ptr<juce::FileChooser> chooser;

  PushButton cancel{"Cancel"};
  PushButton backButton{"Back"};
  PushButton nextButton{"Choose presets", PushButton::Kind::primary};
  PushButton changeSource{"Change source", PushButton::Kind::ghost};
  PushButton chooseFileButton{"Choose dump files..."};
  PushButton chooseFolderButton{"Choose a folder..."};
  PushButton selectAll{"Select all", PushButton::Kind::ghost};
  PushButton selectNone{"Select none", PushButton::Kind::ghost};
  PushButton saveTable{"Save mapping table"};
  PushButton useTable{"Use a mapping table..."};
  PushButton exportButton{"Export report (Markdown)"};
  PushButton openButton{"Open preset", PushButton::Kind::primary};
  SelectBox setlistBox{"Import into setlist"};
  Dialog prompt;
};

}  // namespace pedaleira::ui
