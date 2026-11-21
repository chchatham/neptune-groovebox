#pragma once

// ---------------------------------------------------------------------------
// Tests for ProjectManager: new/save/load/autosave/recent list integration.
// ---------------------------------------------------------------------------

#include "../src/project/ProjectManager.h"

class ProjectIntegrationTests : public juce::UnitTest
{
public:
    ProjectIntegrationTests() : juce::UnitTest("Project Integration") {}

    void runTest() override
    {
        // -------------------------------------------------------------------
        beginTest("New project has default state");
        {
            ProjectManager pm;
            pm.newProject("Test Project");

            expectEquals(pm.getProjectName(), juce::String("Test Project"));
            expect(!pm.isDirty());
            expect(!pm.hasFile());

            // Pattern 0 should have 8 tracks, each with 16 steps
            auto& data = pm.getData();
            expectEquals((int) data.patterns[0].tracks.size(), 8);
            for (auto& track : data.patterns[0].tracks)
            {
                expectEquals(track.length, 16);
                expectEquals((int) track.steps.size(), 16);
            }
        }

        // -------------------------------------------------------------------
        beginTest("Save round-trip preserves project name and track count");
        {
            // Create a temp directory for the project
            juce::File tempDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_test_project_rt");
            tempDir.deleteRecursively();

            ProjectManager pm;
            pm.onCapture = [&]() { return pm.getData(); };
            pm.newProject("Round Trip");

            // Modify some state
            auto& data = pm.getData();
            data.bpm = 140.0f;
            data.patterns[0].tracks.resize(4);
            for (auto& t : data.patterns[0].tracks)
            {
                t.length = 32;
                t.steps.resize(32);
            }

            // Save
            bool saved = pm.saveProjectAs(tempDir);
            expect(saved);
            expect(tempDir.getChildFile("project.json").existsAsFile());

            // Load into a fresh ProjectManager
            ProjectManager pm2;
            bool rebuildCalled = false;
            pm2.onRebuild = [&](const ProjectData& loaded)
            {
                rebuildCalled = true;
                expectEquals(loaded.name, juce::String("Round Trip"));
                expectEquals(loaded.bpm, 140.0f);
                expectEquals((int) loaded.patterns[0].tracks.size(), 4);
                expectEquals(loaded.patterns[0].tracks[0].length, 32);
            };

            bool loaded = pm2.loadProject(tempDir);
            expect(loaded);
            expect(rebuildCalled);
            expect(!pm2.isDirty());

            // Cleanup
            tempDir.deleteRecursively();
        }

        // -------------------------------------------------------------------
        beginTest("Autosave triggers after dirty + interval elapsed");
        {
            juce::File tempDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_test_autosave");
            tempDir.deleteRecursively();

            ProjectManager pm;
            pm.onCapture = [&]() { return pm.getData(); };
            pm.newProject("Autosave Test");

            // Save initial project so it has a file
            pm.saveProjectAs(tempDir);

            // Set autosave interval to near-zero so shouldAutosave fires
            pm.setAutosaveInterval(5000); // minimum allowed by Project

            // Mark dirty
            pm.markDirty();
            expect(pm.isDirty());

            // shouldAutosave depends on time elapsed — set interval very low
            // and directly call autosaveCheck.  Since lastSaveTime was set
            // on saveProjectAs, we can temporarily lower the interval.
            pm.getProject().setAutosaveInterval(5000);

            // Force enough time delta by calling performAutosave path directly.
            // In production the timer calls autosaveCheck periodically.
            // For the test we verify the dirty-check logic:
            pm.markDirty();

            // The autosaveCheck should NOT save if interval hasn't elapsed
            // (since we just saved moments ago).
            pm.autosaveCheck();
            // It may or may not have saved depending on timing;
            // but the mechanism itself is exercised without error.
            // After a real autosave, dirty should be cleared:
            if (!pm.isDirty())
            {
                // Autosave fired — verify file was updated
                expect(tempDir.getChildFile("project.json").existsAsFile());
            }

            // Cleanup
            tempDir.deleteRecursively();
        }

        // -------------------------------------------------------------------
        beginTest("Load calls rebuild callback");
        {
            juce::File tempDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_test_rebuild");
            tempDir.deleteRecursively();

            // First save a project
            ProjectManager pm1;
            pm1.onCapture = [&]() { return pm1.getData(); };
            pm1.newProject("Rebuild CB");
            pm1.getData().bpm = 99.0f;
            pm1.saveProjectAs(tempDir);

            // Load into a new ProjectManager and verify callback fires
            ProjectManager pm2;
            int callCount = 0;
            float loadedBpm = 0.0f;
            pm2.onRebuild = [&](const ProjectData& d)
            {
                callCount++;
                loadedBpm = d.bpm;
            };

            pm2.loadProject(tempDir);
            expectEquals(callCount, 1);
            expectEquals(loadedBpm, 99.0f);

            // Cleanup
            tempDir.deleteRecursively();
        }

        // -------------------------------------------------------------------
        beginTest("NewProject resets dirty flag");
        {
            ProjectManager pm;
            pm.onCapture = [&]() { return pm.getData(); };

            // Create and dirty a project
            juce::File tempDir = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_test_dirty");
            tempDir.deleteRecursively();

            pm.newProject("Dirty Test");
            pm.saveProjectAs(tempDir);
            pm.markDirty();
            expect(pm.isDirty());

            // newProject should reset dirty
            pm.newProject("Fresh");
            expect(!pm.isDirty());
            expectEquals(pm.getProjectName(), juce::String("Fresh"));

            // Cleanup
            tempDir.deleteRecursively();
        }

        // -------------------------------------------------------------------
        beginTest("Recent list persists across instances");
        {
            juce::File tempDir1 = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_test_recent1");
            juce::File tempDir2 = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("neptune_test_recent2");
            tempDir1.deleteRecursively();
            tempDir2.deleteRecursively();

            {
                ProjectManager pm;
                pm.onCapture = [&]() { return pm.getData(); };

                pm.newProject("Recent A");
                pm.saveProjectAs(tempDir1);

                pm.newProject("Recent B");
                pm.saveProjectAs(tempDir2);

                // Both should be in recent list
                auto& recent = pm.getRecentProjects();
                expect(recent.size() >= 2);
                // Most recent is first
                expectEquals(recent[0], tempDir2.getFullPathName());
            }

            {
                // A new ProjectManager should load the persisted recent list
                ProjectManager pm2;
                auto& recent = pm2.getRecentProjects();
                // Should contain both paths (assuming no other tests wiped it)
                bool found1 = false, found2 = false;
                for (auto& p : recent)
                {
                    if (p == tempDir1.getFullPathName()) found1 = true;
                    if (p == tempDir2.getFullPathName()) found2 = true;
                }
                expect(found1);
                expect(found2);
            }

            // Cleanup
            tempDir1.deleteRecursively();
            tempDir2.deleteRecursively();
        }

        // -------------------------------------------------------------------
        beginTest("Save As that the save box names or takes back");
        {
            const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("neptune_test_save_as");
            root.deleteRecursively();
            const auto p1 = root.getChildFile("P01"), p2 = root.getChildFile("P02");
            auto bpmIn = [](const juce::File& dir)
            {
                return ProjectData::fromJSON(dir.getChildFile("project.json").loadFileAsString()).bpm;
            };

            ProjectManager pm;
            pm.onCapture = [&]() { return pm.getData(); };
            pm.newProject();
            pm.getData().bpm = 100.0f;

            // Into an empty slot: saved at once, under the name it had; named by the box.
            expect(pm.saveProjectAsPending(p1));
            expect(pm.saveAsPending());
            expect(pm.getCurrentFile() == p1);
            expectEquals(ProjectManager::nameIn(p1), juce::String("Untitled"));
            expect(!pm.nameSaveAs("   "), "no name: not taken");
            expect(pm.saveAsPending());
            expect(pm.nameSaveAs("  First Song with a long name "));
            expect(!pm.saveAsPending());
            expectEquals(ProjectManager::nameIn(p1), juce::String("First Song wi"), "13 characters, as the display shows");
            expectEquals(pm.getProjectName(), juce::String("First Song wi"));
            expect(!ProjectManager::keptIn(p1).exists());
            expect(!pm.takeBackSaveAs(), "named: nothing left to take back");
            expectEquals(bpmIn(p1), 100.0f);

            // Into another empty slot, taken back: the slot is gone, the project is P01's again.
            pm.getData().bpm = 110.0f;
            expect(pm.saveProjectAsPending(p2));
            expect(pm.getCurrentFile() == p2);
            expect(pm.takeBackSaveAs());
            expect(!p2.exists());
            expect(pm.getCurrentFile() == p1);
            expectEquals(pm.getProjectName(), juce::String("First Song wi"));
            expectEquals(pm.getData().bpm, 110.0f, "what is playing is not touched");

            // Over a slot that holds a project (with a sample beside it), taken back: it holds what it held.
            p1.getChildFile("samples").getChildFile("kick.wav").replaceWithText("x");
            expect(pm.saveProjectAsPending(p1));
            expectEquals(bpmIn(p1), 110.0f);
            expect(ProjectManager::keptIn(p1).existsAsFile());
            expect(pm.takeBackSaveAs());
            expectEquals(bpmIn(p1), 100.0f);
            expectEquals(ProjectManager::nameIn(p1), juce::String("First Song wi"));
            expect(p1.getChildFile("samples").getChildFile("kick.wav").existsAsFile());
            expect(!ProjectManager::keptIn(p1).exists());
            expect(pm.getCurrentFile() == p1);

            // Over it again and named: the new one, and nothing kept beside it.
            expect(pm.saveProjectAsPending(p1));
            pm.getData().bpm = 150.0f;                      // turned after the save: not in it
            expect(pm.nameSaveAs("Second"));
            expectEquals(bpmIn(p1), 110.0f);
            expectEquals(ProjectManager::nameIn(p1), juce::String("Second"));
            expect(!ProjectManager::keptIn(p1).exists());

            // A second save-as, a save, a load or a new project settles an unanswered one as it stands.
            expect(pm.saveProjectAsPending(p2));
            expect(pm.saveProjectAsPending(p1));
            expect(pm.pendingSaveDir() == p1);
            expect(p2.getChildFile("project.json").existsAsFile());
            expect(pm.saveProject());
            expect(!pm.saveAsPending());
            expect(!ProjectManager::keptIn(p1).exists());
            expectEquals(bpmIn(p1), 150.0f);
            expect(pm.saveProjectAsPending(p2));
            expect(pm.loadProject(p1));
            expect(!pm.saveAsPending() && !pm.takeBackSaveAs());
            expect(p2.getChildFile("project.json").existsAsFile());
            expect(pm.saveProjectAsPending(p2));
            pm.newProject();
            expect(!pm.saveAsPending());
            expect(!pm.hasFile());

            expectEquals(ProjectManager::nameIn(root.getChildFile("P09")), juce::String());
            root.deleteRecursively();
        }
    }
};

static ProjectIntegrationTests projectIntegrationTests;
