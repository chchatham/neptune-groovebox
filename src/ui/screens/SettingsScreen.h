#pragma once

#include "../GridScreen.h"
#include <functional>

// -----------------------------------------------------------------------------
// SETTINGS: three pages in the standard grid, one page key (Func + top-row pad) each.
//
//   SETUP   BPM  SWNG QNT  CLK  | MVOL PAT  HELP ---      (HELP: the Shift / Func help cards)
//   MASTR   LOW  MID  MFRQ HIGH | THR  RATI MAKE CEIL     (master EQ, compressor, limiter)
//   PROJ    SLOT LOAD SAVE SVAS | NEW  AUTO ---  ---
//
// Projects live in numbered slots (there is no keyboard to type names): SLOT picks one, and the
// strip at the bottom says whether it holds a project and which slot the current project came
// from. LOAD / SAVE / SVAS / NEW are actions: four clicks clockwise on the knob, spread over at
// least 300 ms (Screen::TurnConfirm), so a brushed knob never overwrites or discards anything;
// the cell shows the progress and then OK or FAIL. Pads are dark on this screen.
//
// The screen knows nothing about the app: everything goes through the hooks below.
// -----------------------------------------------------------------------------
class SettingsScreen : public GridScreen
{
public:
    static constexpr int NUM_SLOTS = 32;
    static constexpr int ACTION_STEPS = TurnConfirm::STEPS;      // clockwise clicks needed to run an action

    struct Hooks
    {
        // SETUP
        std::function<float()> getTempo;            std::function<void(float)> setTempo;
        std::function<float()> getSwing;            std::function<void(float)> setSwing;
        std::function<float()> getQuantize;         std::function<void(float)> setQuantize;
        std::function<int()> getClockMode;          std::function<void(int)> setClockMode;     // 0 INT, 1 EXT, 2 AUTO, 3 LINK
        std::function<float()> getMasterLevel;      std::function<void(float)> setMasterLevel;
        std::function<int()> getPattern;            std::function<void(int)> setPattern;       // 0-based
        std::function<int()> getHelpMode;           std::function<void(int)> setHelpMode;      // 0 OFF, 1 0.35S, 2 INST
        // MASTR: index 0..7 = LOW MID MFRQ HIGH THR RATI MAKE CEIL
        std::function<float(int)> getMaster;        std::function<void(int, float)> setMaster;
        // PROJ
        std::function<bool(int)> slotUsed;          // slot is 0-based
        std::function<int()> currentSlot;           // -1 = the current project has never been saved
        std::function<bool(int)> loadSlot;
        std::function<bool(int)> saveToSlot;        // save as
        std::function<bool()> saveCurrent;          // false if never saved
        std::function<void()> newProject;
        std::function<bool()> getAutosave;          std::function<void(bool)> setAutosave;
    };

    explicit SettingsScreen(Hooks h = {}) : hooks(std::move(h))
    {
        buildPages();
    }

    void setHooks(Hooks h) { hooks = std::move(h); }

    juce::String getName() const override { return gridPages[(size_t) gridPageIndex].name; }
    juce::String getSubPage() const override { return "SETTING"; }      // seven letters: fits beside "SETUP T1" and "1/3"

    void onActivate() override
    {
        resetActions();
        if (hooks.currentSlot && hooks.currentSlot() >= 0)
            selectedSlot = hooks.currentSlot();
    }

    void onDeactivate() override { resetActions(); }

    void onEncoderChange(int encoderIndex, int value) override
    {
        // The project actions (PROJ, cells 2-5) are confirmed by four clicks; touching any other
        // cell cancels a half-turned action.
        const bool action = gridPages[(size_t) gridPageIndex].name == "PROJ" && encoderIndex >= 1 && encoderIndex <= 4;
        if (!action)
        {
            confirm.cancel();
            GridScreen::onEncoderChange(encoderIndex, value);
            return;
        }
        const auto which = (Action) (encoderIndex - 1);
        for (auto& r : actionResult) r = 0;
        if (confirm.step(encoderIndex, value > 64 ? value - 128 : value, clock ? clock() : -1))
            actionResult[(size_t) which] = runAction(which) ? 1 : -1;
    }

    void onArrowLeft() override  { resetActions(); GridScreen::onArrowLeft(); }
    void onArrowRight() override { resetActions(); GridScreen::onArrowRight(); }

    // Page keys: SETUP, MASTR, PROJ.
    int getPageKeyCount() const override { return (int) gridPages.size(); }
    void onPageKey(int index) override
    {
        if (index >= 0 && index < (int) gridPages.size()) { resetActions(); setPageIndex(index); }
    }
    int getActivePageKey() const override { return gridPageIndex; }
    juce::String getPageKeyName(int index) const override
    {
        return (index >= 0 && index < (int) gridPages.size()) ? gridPages[(size_t) index].name : juce::String();
    }

    // No pad does anything here: all dark.
    bool getPadColours(std::array<PadColour, 16>& out) const override { out.fill({}); return true; }

    // The clock used by the action confirmation (tests inject their own).
    void setClock(std::function<juce::int64()> c) { clock = std::move(c); }

    int getSelectedSlot() const { return selectedSlot; }
    void setSelectedSlot(int s) { selectedSlot = juce::jlimit(0, NUM_SLOTS - 1, s); }
    juce::String getStatusLine() const { return statusLine(); }

protected:
    // PROJ shows the slot status under the grid; the other pages the page marks.
    void drawStrip(DisplayRenderer& display) override
    {
        if (gridPages[(size_t) gridPageIndex].name == "PROJ")
            display.drawString(1, ParamGrid::STRIP_Y - 1, statusLine());
        else
            GridScreen::drawStrip(display);
    }

private:
    enum Action { Load = 0, Save, SaveAs, New, NumActions };

    static juce::String slotName(int slot) { return "P" + juce::String(slot + 1).paddedLeft('0', 2); }

    juce::String statusLine() const
    {
        juce::String s = slotName(selectedSlot) + (hooks.slotUsed && hooks.slotUsed(selectedSlot) ? " USED" : " EMPTY");
        int current = hooks.currentSlot ? hooks.currentSlot() : -1;
        s += " NOW " + (current >= 0 ? slotName(current) : juce::String("UNSAVED"));     // 21 characters at most
        return s;
    }

    void resetActions()
    {
        confirm.cancel();
        for (auto& r : actionResult) r = 0;
    }

    bool runAction(Action a)
    {
        switch (a)
        {
            case Load:   return hooks.loadSlot && hooks.loadSlot(selectedSlot);
            case SaveAs: return hooks.saveToSlot && hooks.saveToSlot(selectedSlot);
            case Save:
                // A project that has never been saved goes to the selected slot.
                if (hooks.saveCurrent && hooks.saveCurrent()) return true;
                return hooks.saveToSlot && hooks.saveToSlot(selectedSlot);
            case New:
                if (!hooks.newProject) return false;
                hooks.newProject();
                return true;
            default: return false;
        }
    }

    // A cell that runs `action` once its knob has been confirmed (see onEncoderChange).
    Cell actionCell(const char* label, Action action, int cellIndex)
    {
        return cell({ label, 0, 0, (float) ACTION_STEPS, 0, ParamKind::Linear },
                    [this, cellIndex] { return confirm.knob == cellIndex ? (float) confirm.steps : 0.0f; },
                    [](float) {},
                    [this, action, cellIndex](float)
                    {
                        if (actionResult[(size_t) action] > 0) return juce::String("OK");
                        if (actionResult[(size_t) action] < 0) return juce::String("FAIL");
                        return confirm.knob == cellIndex && confirm.steps > 0 ? confirm.progress() : juce::String("TURN>");
                    });
    }

    void buildPages()
    {
        auto L = ParamKind::Linear; auto G = ParamKind::Log; auto I = ParamKind::Int; auto C = ParamKind::Choice;
        static const char* const clocks[] = { "INT", "EXT", "AUTO", "LINK" };

        Page setup;
        setup.name = "SETUP";
        setup.cells[0] = cell({ "BPM", 0, 20, 300, 120, L, "bpm" },
                              [this] { return hooks.getTempo ? hooks.getTempo() : 120.0f; },
                              [this](float v) { if (hooks.setTempo) hooks.setTempo(v); });
        setup.cells[1] = cell({ "SWNG", 0, 0, 1, 0, L },
                              [this] { return hooks.getSwing ? hooks.getSwing() : 0.0f; },
                              [this](float v) { if (hooks.setSwing) hooks.setSwing(v); });
        setup.cells[2] = cell({ "QNT", 0, 0, 1, 0, L },
                              [this] { return hooks.getQuantize ? hooks.getQuantize() : 0.0f; },
                              [this](float v) { if (hooks.setQuantize) hooks.setQuantize(v); });
        setup.cells[3] = cell({ "CLK", 0, 0, 3, 0, C, "", clocks, 4 },
                              [this] { return hooks.getClockMode ? (float) hooks.getClockMode() : 0.0f; },
                              [this](float v) { if (hooks.setClockMode) hooks.setClockMode((int) v); });
        setup.cells[4] = cell({ "MVOL", 0, 0, 1, 0.8f, L },
                              [this] { return hooks.getMasterLevel ? hooks.getMasterLevel() : 0.8f; },
                              [this](float v) { if (hooks.setMasterLevel) hooks.setMasterLevel(v); });
        setup.cells[5] = cell({ "PAT", 0, 1, 16, 1, I },
                              [this] { return hooks.getPattern ? (float) (hooks.getPattern() + 1) : 1.0f; },
                              [this](float v) { if (hooks.setPattern) hooks.setPattern((int) v - 1); });
        // The help cards (hold Shift or Func alone): never, after 0.35 s, or at once.
        static const char* const helps[] = { "OFF", "0.35S", "INST" };
        setup.cells[6] = cell({ "HELP", 0, 0, 2, 1, C, "", helps, 3 },
                              [this] { return hooks.getHelpMode ? (float) hooks.getHelpMode() : 1.0f; },
                              [this](float v) { if (hooks.setHelpMode) hooks.setHelpMode((int) std::lround(v)); });
        gridPages.push_back(setup);

        Page master;
        master.name = "MASTR";
        const ParamSpec masterSpecs[8] = {
            { "LOW",  0, -18, 18, 0, L, "dB" },
            { "MID",  0, -18, 18, 0, L, "dB" },
            { "MFRQ", 0, 100, 8000, 1000, G, "Hz" },
            { "HIGH", 0, -18, 18, 0, L, "dB" },
            { "THR",  0, -60, 0, 0, L, "dB" },
            { "RATI", 0, 1, 20, 2, G, "x" },
            { "MAKE", 0, 0, 30, 0, L, "dB" },
            { "CEIL", 0, -3, 0, -0.3f, L, "dB" } };
        for (int i = 0; i < 8; ++i)
            master.cells[(size_t) i] = cell(masterSpecs[i],
                                            [this, i, d = masterSpecs[i].defaultVal] { return hooks.getMaster ? hooks.getMaster(i) : d; },
                                            [this, i](float v) { if (hooks.setMaster) hooks.setMaster(i, v); });
        gridPages.push_back(master);

        Page proj;
        proj.name = "PROJ";
        proj.cells[0] = cell({ "SLOT", 0, 0, NUM_SLOTS - 1, 0, I },
                             [this] { return (float) selectedSlot; },
                             [this](float v) { selectedSlot = (int) v; resetActions(); },
                             [this](float) { return slotName(selectedSlot); });
        proj.cells[1] = actionCell("LOAD", Load, 1);
        proj.cells[2] = actionCell("SAVE", Save, 2);
        proj.cells[3] = actionCell("SVAS", SaveAs, 3);
        proj.cells[4] = actionCell("NEW", New, 4);
        proj.cells[5] = cell(onOff("AUTO"),
                             [this] { return (hooks.getAutosave && hooks.getAutosave()) ? 1.0f : 0.0f; },
                             [this](float v) { if (hooks.setAutosave) hooks.setAutosave(v > 0.5f); });
        gridPages.push_back(proj);
    }

    Hooks hooks;
    int selectedSlot = 0;
    TurnConfirm confirm;
    std::function<juce::int64()> clock;
    int actionResult[NumActions] = {};      // 1 = OK, -1 = FAIL
};
