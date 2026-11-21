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
        keepSaveAs();
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

        keepSaveAs();
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

    // --- Save As that can be named or taken back ----------------------------
    //
    // The controller saves at once (nothing waits for the Mac); the Mac's save box then names the
    // save or takes it back. Until one of the two - or anything else settles it (keepSaveAs) -
    // the project file the folder held before is kept in it, as "project.json.before".

    static constexpr int MAX_NAME_LENGTH = 13;      // what SETTINGS -> PROJ shows after "P05 NOW "

    // What a typed name becomes: printable ASCII (all the display's font has), no outer spaces.
    static juce::String cleanName(const juce::String& typed)
    {
        juce::String out;
        for (auto c : typed)
            if (c >= 32 && c <= 126)
                out += juce::String::charToString(c);
        return out.trim().substring(0, MAX_NAME_LENGTH).trim();
    }

    static juce::File keptIn(const juce::File& projectDir) { return projectDir.getChildFile("project.json.before"); }

    // The name of the project saved in a folder; empty if there is none.
    static juce::String nameIn(const juce::File& projectDir)
    {
        const auto file = projectDir.getChildFile("project.json");
        if (!file.existsAsFile())
            return {};
        return juce::JSON::parse(file.loadFileAsString()).getProperty("name", {}).toString();
    }

    bool saveAsPending() const { return pending.active; }
    juce::File pendingSaveDir() const { return pending.active ? pending.dir : juce::File(); }

    /** Save As, keeping what the folder held and which project this was, for takeBackSaveAs(). */
    bool saveProjectAsPending(const juce::File& projectDir)
    {
        keepSaveAs();
        const auto before = keptIn(projectDir);
        const auto file = projectDir.getChildFile("project.json");
        before.deleteFile();
        const bool hadFolder = projectDir.isDirectory();
        const bool hadOne = file.existsAsFile();
        if (hadOne && !file.copyFileTo(before))
            return false;

        Pending p;
        p.active = true;
        p.dir = projectDir;
        p.hadOne = hadOne;
        p.hadFolder = hadFolder;
        p.previousFile = project.getCurrentFile();
        p.previousName = project.getData().name;
        p.previousRecent = project.getRecentProjects();

        if (!saveProjectAs(projectDir))
        {
            pending = p;
            takeBackSaveAs();
            return false;
        }
        pending = p;
        return true;
    }

    /** The save stays, under this name. False (and nothing changed) if the name is not usable. */
    bool nameSaveAs(const juce::String& typed)
    {
        const auto name = cleanName(typed);
        if (!pending.active || name.isEmpty())
            return false;
        // Only the name is rewritten: the save holds what was playing when it was made.
        const auto file = pending.dir.getChildFile("project.json");
        auto saved = ProjectData::fromJSON(file.loadFileAsString());
        saved.name = name;
        if (!file.replaceWithText(saved.toJSON()))
            return false;
        if (project.getCurrentFile() == pending.dir)
            project.getData().name = name;
        keepSaveAs();
        return true;
    }

    /** The save stays as it is (also: quitting, or another project action, with the box open). */
    void keepSaveAs()
    {
        if (!pending.active)
            return;
        keptIn(pending.dir).deleteFile();
        pending = {};
    }

    /** The save never happened: the folder holds what it held, the project is the one it was. */
    bool takeBackSaveAs()
    {
        if (!pending.active)
            return false;
        const auto p = pending;
        pending = {};
        const auto file = p.dir.getChildFile("project.json");
        bool ok = true;
        if (p.hadOne)             ok = keptIn(p.dir).moveFileTo(file);      // (replaces the save)
        else if (p.hadFolder)     ok = file.deleteFile();
        else                      ok = p.dir.deleteRecursively();
        // (only if the project is still the one that save made: a load since then stands)
        if (project.getCurrentFile() == p.dir)
        {
            project.setCurrentFile(p.previousFile);
            project.getData().name = p.previousName;
            project.setRecentProjects(p.previousRecent);
            saveRecentList();
        }
        return ok;
    }

    // --- Load ---------------------------------------------------------------

    bool loadProject(const juce::File& projectDir)
    {
        keepSaveAs();
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

    struct Pending
    {
        bool active = false;
        bool hadOne = false;                 // the folder held a project before
        bool hadFolder = false;
        juce::File dir, previousFile;
        juce::String previousName;
        std::vector<juce::String> previousRecent;
    };
    Pending pending;
};
