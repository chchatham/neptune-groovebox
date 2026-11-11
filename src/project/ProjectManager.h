#pragma once

#include "Project.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>

// ---------------------------------------------------------------------------
// ProjectManager — orchestrates Project save/load/autosave on message thread.
//
// Owns a Project instance and provides:
//   - newProject()      — reset to defaults, calls onRebuild callback
//   - saveProject()     — captures state via onCapture callback, saves to disk
//   - loadProject(file) — loads from disk, calls onRebuild callback
//   - autosaveCheck()   — called from a timer; saves if dirty + interval elapsed
//
// Callbacks:
//   onCapture:  std::function<ProjectData()>
//               Called before save to snapshot the live engine/sequencer state.
//   onRebuild:  std::function<void(const ProjectData&)>
//               Called after load/new to push state into engine/sequencer.
//
// Thread safety: all public methods run on the message thread.  The autosave
// timer (juce::Timer) fires on the message thread, so no cross-thread issues.
// ---------------------------------------------------------------------------

class ProjectManager : private juce::Timer
{
public:
    // --- Callbacks set by the app at startup --------------------------------

    std::function<ProjectData()> onCapture;
    std::function<void(const ProjectData&)> onRebuild;

    // --- Lifecycle ----------------------------------------------------------

    ProjectManager()
    {
        loadRecentList();
    }

    ~ProjectManager() override
    {
        stopTimer();
    }

    // --- New project --------------------------------------------------------

    void newProject(const juce::String& name = "Untitled")
    {
        project.newProject(name);

        // Initialise 8 tracks in pattern 0 with 16 steps each
        auto& data = project.getData();
        auto& pat = data.patterns[0];
        pat.tracks.resize(8);
        for (auto& track : pat.tracks)
        {
            track.length = 16;
            track.steps.resize(16);
        }

        if (onRebuild)
            onRebuild(project.getData());
    }

    // --- Save ---------------------------------------------------------------

    /** Save to the current project directory. Returns false if no file set. */
    bool saveProject()
    {
        if (!project.hasFile())
            return false;

        captureState();
        return project.save(project.getCurrentFile());
    }

    /** Save to a specific directory (Save As). */
    bool saveProjectAs(const juce::File& projectDir)
    {
        captureState();
        bool ok = project.save(projectDir);
        if (ok)
            saveRecentList();
        return ok;
    }

    // --- Load ---------------------------------------------------------------

    bool loadProject(const juce::File& projectDir)
    {
        if (!project.load(projectDir))
            return false;

        saveRecentList();

        if (onRebuild)
            onRebuild(project.getData());

        return true;
    }

    // --- Autosave -----------------------------------------------------------

    /** Start the autosave timer. Call once after wiring callbacks. */
    void startAutosave(int checkIntervalMs = 5000)
    {
        startTimer(checkIntervalMs);
    }

    void stopAutosave()
    {
        stopTimer();
    }

    /** Called from the timer callback. Safe to call manually. */
    void autosaveCheck()
    {
        if (project.shouldAutosave())
        {
            captureState();
            project.performAutosave();
        }
    }

    // --- Dirty flag ---------------------------------------------------------

    void markDirty() { project.markDirty(); }
    bool isDirty() const { return project.isDirty(); }

    // --- Accessors ----------------------------------------------------------

    Project& getProject() { return project; }
    const Project& getProject() const { return project; }

    ProjectData& getData() { return project.getData(); }
    const ProjectData& getData() const { return project.getData(); }

    juce::String getProjectName() const { return project.getProjectName(); }
    bool hasFile() const { return project.hasFile(); }
    juce::File getCurrentFile() const { return project.getCurrentFile(); }

    // --- Recent projects ----------------------------------------------------

    const std::vector<juce::String>& getRecentProjects() const
    {
        return project.getRecentProjects();
    }

    void clearRecentProjects()
    {
        project.clearRecent();
        saveRecentList();
    }

    // --- Autosave interval --------------------------------------------------

    void setAutosaveInterval(int ms) { project.setAutosaveInterval(ms); }
    int getAutosaveInterval() const { return project.getAutosaveInterval(); }

private:
    Project project;

    // --- Timer callback (message thread) ------------------------------------

    void timerCallback() override
    {
        autosaveCheck();
    }

    // --- State capture helper -----------------------------------------------

    void captureState()
    {
        if (onCapture)
            project.getData() = onCapture();
    }

    // --- Recent list persistence --------------------------------------------

    juce::File getAppDataDir() const
    {
        auto dir = juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
            .getChildFile("NeptuneGroovebox");
        if (!dir.isDirectory())
            dir.createDirectory();
        return dir;
    }

    void saveRecentList()
    {
        project.saveRecentList(getAppDataDir());
    }

    void loadRecentList()
    {
        project.loadRecentList(getAppDataDir());
    }
};
