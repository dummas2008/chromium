// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/command_line.h"
#include "base/macros.h"
#include "base/path_service.h"
#include "base/process/process.h"
#include "base/test/test_timeouts.h"
#include "build/build_config.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/devtools/devtools_window.h"
#include "chrome/browser/search/search.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/url_constants.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/test_switches.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_iterator.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test_utils.h"
#include "net/base/filename_util.h"
#include "net/test/embedded_test_server/embedded_test_server.h"

using content::RenderViewHost;
using content::RenderWidgetHost;
using content::WebContents;

namespace {

int RenderProcessHostCount() {
  content::RenderProcessHost::iterator hosts =
      content::RenderProcessHost::AllHostsIterator();
  int count = 0;
  while (!hosts.IsAtEnd()) {
    if (hosts.GetCurrentValue()->HasConnection())
      count++;
    hosts.Advance();
  }
  return count;
}

WebContents* FindFirstDevToolsContents() {
  scoped_ptr<content::RenderWidgetHostIterator> widgets(
      RenderWidgetHost::GetRenderWidgetHosts());
  while (content::RenderWidgetHost* widget = widgets->GetNextHost()) {
    if (!widget->GetProcess()->HasConnection())
      continue;
    RenderViewHost* view_host = RenderViewHost::From(widget);
    if (!view_host)
      continue;
    WebContents* contents = WebContents::FromRenderViewHost(view_host);
    GURL url = contents->GetURL();
    if (url.SchemeIs(content::kChromeDevToolsScheme))
      return contents;
  }
  return NULL;
}

// TODO(rvargas) crbug.com/417532: Remove this code.
base::Process ProcessFromHandle(base::ProcessHandle handle) {
#if defined(OS_WIN)
  if (handle == GetCurrentProcess())
    return base::Process::Current();

  base::ProcessHandle out_handle;
  if (!::DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                         &out_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    return base::Process();
  }
  handle = out_handle;
#endif  // defined(OS_WIN)
  return base::Process(handle);
}

}  // namespace

class ChromeRenderProcessHostTest : public InProcessBrowserTest {
 public:
  ChromeRenderProcessHostTest() {}

  // Show a tab, activating the current one if there is one, and wait for
  // the renderer process to be created or foregrounded, returning the process
  // handle.
  base::Process ShowSingletonTab(const GURL& page) {
    chrome::ShowSingletonTab(browser(), page);
    WebContents* wc = browser()->tab_strip_model()->GetActiveWebContents();
    CHECK(wc->GetURL() == page);

    WaitForLauncherThread();
    WaitForMessageProcessing(wc);
    return ProcessFromHandle(wc->GetRenderProcessHost()->GetHandle());
  }

  // Loads the given url in a new background tab and returns the handle of its
  // renderer.
  base::Process OpenBackgroundTab(const GURL& page) {
    ui_test_utils::NavigateToURLWithDisposition(
        browser(), page, NEW_BACKGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_NAVIGATION);

    TabStripModel* tab_strip = browser()->tab_strip_model();
    WebContents* wc =
        tab_strip->GetWebContentsAt(tab_strip->active_index() + 1);
    CHECK(wc->GetVisibleURL() == page);

    WaitForLauncherThread();
    WaitForMessageProcessing(wc);
    return ProcessFromHandle(wc->GetRenderProcessHost()->GetHandle());
  }

  // Ensures that the backgrounding / foregrounding gets a chance to run.
  void WaitForLauncherThread() {
    content::BrowserThread::PostTaskAndReply(
        content::BrowserThread::PROCESS_LAUNCHER, FROM_HERE,
        base::Bind(&base::DoNothing), base::MessageLoop::QuitWhenIdleClosure());
    base::MessageLoop::current()->Run();
  }

  // Implicitly waits for the renderer process associated with the specified
  // WebContents to process outstanding IPC messages by running some JavaScript
  // and waiting for the result.
  void WaitForMessageProcessing(WebContents* wc) {
    bool result = false;
    ASSERT_TRUE(content::ExecuteScriptAndExtractBool(
        wc, "window.domAutomationController.send(true);", &result));
    ASSERT_TRUE(result);
  }

  // When we hit the max number of renderers, verify that the way we do process
  // sharing behaves correctly.  In particular, this test is verifying that even
  // when we hit the max process limit, that renderers of each type will wind up
  // in a process of that type, even if that means creating a new process.
  void TestProcessOverflow() {
    int tab_count = 1;
    int host_count = 1;
    WebContents* tab1 = NULL;
    WebContents* tab2 = NULL;
    content::RenderProcessHost* rph1 = NULL;
    content::RenderProcessHost* rph2 = NULL;
    content::RenderProcessHost* rph3 = NULL;

    // Change the first tab to be the omnibox page (TYPE_WEBUI).
    GURL omnibox(chrome::kChromeUIOmniboxURL);
    ui_test_utils::NavigateToURL(browser(), omnibox);
    EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
    tab1 = browser()->tab_strip_model()->GetWebContentsAt(tab_count - 1);
    rph1 = tab1->GetRenderProcessHost();
    EXPECT_EQ(omnibox, tab1->GetURL());
    EXPECT_EQ(host_count, RenderProcessHostCount());

    // Create a new TYPE_TABBED tab.  It should be in its own process.
    GURL page1("data:text/html,hello world1");

    ui_test_utils::WindowedTabAddedNotificationObserver observer1(
        content::NotificationService::AllSources());
    chrome::ShowSingletonTab(browser(), page1);
    observer1.Wait();

    tab_count++;
    host_count++;
    EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
    tab1 = browser()->tab_strip_model()->GetWebContentsAt(tab_count - 1);
    rph2 = tab1->GetRenderProcessHost();
    EXPECT_EQ(tab1->GetURL(), page1);
    EXPECT_EQ(host_count, RenderProcessHostCount());
    EXPECT_NE(rph1, rph2);

    // Create another TYPE_TABBED tab.  It should share the previous process.
    GURL page2("data:text/html,hello world2");
    ui_test_utils::WindowedTabAddedNotificationObserver observer2(
        content::NotificationService::AllSources());
    chrome::ShowSingletonTab(browser(), page2);
    observer2.Wait();
    tab_count++;
    EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
    tab2 = browser()->tab_strip_model()->GetWebContentsAt(tab_count - 1);
    EXPECT_EQ(tab2->GetURL(), page2);
    EXPECT_EQ(host_count, RenderProcessHostCount());
    EXPECT_EQ(tab2->GetRenderProcessHost(), rph2);

    // Create another TYPE_WEBUI tab.  It should share the process with omnibox.
    // Note: intentionally create this tab after the TYPE_TABBED tabs to
    // exercise bug 43448 where extension and WebUI tabs could get combined into
    // normal renderers.
    GURL history(chrome::kChromeUIHistoryURL);
    ui_test_utils::WindowedTabAddedNotificationObserver observer3(
        content::NotificationService::AllSources());
    chrome::ShowSingletonTab(browser(), history);
    observer3.Wait();
    tab_count++;
    EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
    tab2 = browser()->tab_strip_model()->GetWebContentsAt(tab_count - 1);
    EXPECT_EQ(tab2->GetURL(), GURL(history));
    EXPECT_EQ(host_count, RenderProcessHostCount());
    EXPECT_EQ(tab2->GetRenderProcessHost(), rph1);

    // Create a TYPE_EXTENSION tab.  It should be in its own process.
    // (the bookmark manager is implemented as an extension)
    GURL bookmarks(chrome::kChromeUIBookmarksURL);
    ui_test_utils::WindowedTabAddedNotificationObserver observer4(
        content::NotificationService::AllSources());
    chrome::ShowSingletonTab(browser(), bookmarks);
    observer4.Wait();
    tab_count++;
    host_count++;
    EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
    tab1 = browser()->tab_strip_model()->GetWebContentsAt(tab_count - 1);
    rph3 = tab1->GetRenderProcessHost();
    EXPECT_EQ(tab1->GetURL(), bookmarks);
    EXPECT_EQ(host_count, RenderProcessHostCount());
    EXPECT_NE(rph1, rph3);
    EXPECT_NE(rph2, rph3);
  }
};

class ChromeRenderProcessHostTestWithCommandLine
    : public ChromeRenderProcessHostTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitchASCII(switches::kRendererProcessLimit, "1");
  }
};

// Disable on Mac due to ongoing flakiness. (crbug.com/442785)
#if defined(OS_MACOSX)
#define MAYBE_ProcessPerTab DISABLED_ProcessPerTab
#else
#define MAYBE_ProcessPerTab ProcessPerTab
#endif

IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostTest, MAYBE_ProcessPerTab) {
  // Set max renderers to 1 to force running out of processes.
  content::RenderProcessHost::SetMaxRendererProcessCount(1);

  base::CommandLine& parsed_command_line =
      *base::CommandLine::ForCurrentProcess();
  parsed_command_line.AppendSwitch(switches::kProcessPerTab);

  int tab_count = 1;
  int host_count = 1;

  // Change the first tab to be the new tab page (TYPE_WEBUI).
  GURL omnibox(chrome::kChromeUIOmniboxURL);
  ui_test_utils::NavigateToURL(browser(), omnibox);
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  // Create a new TYPE_TABBED tab.  It should be in its own process.
  GURL page1("data:text/html,hello world1");
  ui_test_utils::WindowedTabAddedNotificationObserver observer1(
      content::NotificationService::AllSources());
  chrome::ShowSingletonTab(browser(), page1);
  observer1.Wait();
  tab_count++;
  host_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  // Create another TYPE_TABBED tab.  It should share the previous process.
  GURL page2("data:text/html,hello world2");
  ui_test_utils::WindowedTabAddedNotificationObserver observer2(
      content::NotificationService::AllSources());
  chrome::ShowSingletonTab(browser(), page2);
  observer2.Wait();
  tab_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  // Create another omnibox tab.  It should share the process with the other
  // WebUI.
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), omnibox, NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_NAVIGATION);
  tab_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  // Create another omnibox tab.  It should share the process with the other
  // WebUI.
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), omnibox, NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_NAVIGATION);
  tab_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());
}

// We don't change process priorities on Mac or Posix because the user lacks the
// permission to raise a process' priority even after lowering it.
#if defined(OS_WIN) || defined(OS_LINUX)
IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostTest, Backgrounding) {
  if (!base::Process::CanBackgroundProcesses()) {
    LOG(ERROR) << "Can't background processes";
    return;
  }
  base::CommandLine& parsed_command_line =
      *base::CommandLine::ForCurrentProcess();
  parsed_command_line.AppendSwitch(switches::kProcessPerTab);

  // Change the first tab to be the omnibox page (TYPE_WEBUI).
  GURL omnibox(chrome::kChromeUIOmniboxURL);
  ui_test_utils::NavigateToURL(browser(), omnibox);

  // Create a new tab. It should be foreground.
  GURL page1("data:text/html,hello world1");
  base::Process process1 = ShowSingletonTab(page1);
  ASSERT_TRUE(process1.IsValid());
  EXPECT_FALSE(process1.IsProcessBackgrounded());

  // Create another tab. It should be foreground, and the first tab should
  // now be background.
  GURL page2("data:text/html,hello world2");
  base::Process process2 = ShowSingletonTab(page2);
  ASSERT_TRUE(process2.IsValid());
  EXPECT_NE(process1.Pid(), process2.Pid());
  EXPECT_TRUE(process1.IsProcessBackgrounded());
  EXPECT_FALSE(process2.IsProcessBackgrounded());

  // Load another tab in background. The renderer of the new tab should be
  // backgrounded, while visibility of the other renderers should not change.
  GURL page3("data:text/html,hello world3");
  base::Process process3 = OpenBackgroundTab(page3);
  ASSERT_TRUE(process3.IsValid());
  EXPECT_NE(process3.Pid(), process1.Pid());
  EXPECT_NE(process3.Pid(), process2.Pid());
  EXPECT_TRUE(process1.IsProcessBackgrounded());
  EXPECT_FALSE(process2.IsProcessBackgrounded());
  // TODO(gab): The new background tab should be backgrounded but it currently
  // intentionally isn't per a workaround to https://crbug.com/560446 in
  // RenderProcessHostImpl::OnProcessLaunched().
  EXPECT_FALSE(process3.IsProcessBackgrounded());

  // Navigate back to the first page. Its renderer should be in foreground
  // again while the other renderers should be backgrounded.
  EXPECT_EQ(process1.Pid(), ShowSingletonTab(page1).Pid());
  EXPECT_FALSE(process1.IsProcessBackgrounded());
  EXPECT_TRUE(process2.IsProcessBackgrounded());
  // TODO(gab): Same as above.
  EXPECT_FALSE(process3.IsProcessBackgrounded());

  // TODO(gab): Remove this when https://crbug.com/560446 is fixed, but for now
  // confirm that the correct state is at least achieved when tab #3 is
  // explicitly foregrounded and re-backgrounded.
  EXPECT_EQ(process3.Pid(), ShowSingletonTab(page3).Pid());
  EXPECT_EQ(process1.Pid(), ShowSingletonTab(page1).Pid());
  EXPECT_FALSE(process1.IsProcessBackgrounded());
  EXPECT_TRUE(process2.IsProcessBackgrounded());
  EXPECT_TRUE(process3.IsProcessBackgrounded());
}
#endif

// TODO(nasko): crbug.com/173137
// Disable on Mac 10.9 due to ongoing flakiness. (crbug.com/442785)
#if defined(OS_MACOSX)
#define MAYBE_ProcessOverflow DISABLED_ProcessOverflow
#else
#define MAYBE_ProcessOverflow ProcessOverflow
#endif

IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostTest, MAYBE_ProcessOverflow) {
  // Set max renderers to 1 to force running out of processes.
  content::RenderProcessHost::SetMaxRendererProcessCount(1);
  TestProcessOverflow();
}

// Disable on Mac 10.9 due to ongoing flakiness. (crbug.com/442785)
#if defined(OS_MACOSX)
#define MAYBE_ProcessOverflowCommandLine DISABLED_ProcessOverflowCommandLine
#else
#define MAYBE_ProcessOverflowCommandLine ProcessOverflowCommandLine
#endif

// Variation of the ProcessOverflow test, which is driven through command line
// parameter instead of direct function call into the class.
IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostTestWithCommandLine,
                       MAYBE_ProcessOverflowCommandLine) {
  TestProcessOverflow();
}

// Ensure that DevTools opened to debug DevTools is launched in a separate
// process when --process-per-tab is set. See crbug.com/69873.
IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostTest,
                       DevToolsOnSelfInOwnProcessPPT) {
#if defined(OS_WIN) && defined(USE_ASH)
  // Disable this test in Metro+Ash for now (http://crbug.com/262796).
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kAshBrowserTests))
    return;
#endif

  base::CommandLine& parsed_command_line =
      *base::CommandLine::ForCurrentProcess();
  parsed_command_line.AppendSwitch(switches::kProcessPerTab);

  int tab_count = 1;
  int host_count = 1;

  GURL page1("data:text/html,hello world1");
  ui_test_utils::WindowedTabAddedNotificationObserver observer1(
      content::NotificationService::AllSources());
  chrome::ShowSingletonTab(browser(), page1);
  observer1.Wait();
  tab_count++;
  host_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  // DevTools start in docked mode (no new tab), in a separate process.
  chrome::ToggleDevToolsWindow(browser(), DevToolsToggleAction::Inspect());
  host_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  WebContents* devtools = FindFirstDevToolsContents();
  DCHECK(devtools);

  // DevTools start in a separate process.
  DevToolsWindow::OpenDevToolsWindow(devtools, DevToolsToggleAction::Inspect());
  host_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  // close docked devtools
  content::WindowedNotificationObserver close_observer(
      content::NOTIFICATION_WEB_CONTENTS_DESTROYED,
      content::Source<WebContents>(devtools));

  chrome::ToggleDevToolsWindow(browser(), DevToolsToggleAction::Toggle());
  close_observer.Wait();
}

// Ensure that DevTools opened to debug DevTools is launched in a separate
// process. See crbug.com/69873.
IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostTest,
                       DevToolsOnSelfInOwnProcess) {
#if defined(OS_WIN) && defined(USE_ASH)
  // Disable this test in Metro+Ash for now (http://crbug.com/262796).
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kAshBrowserTests))
    return;
#endif

  int tab_count = 1;
  int host_count = 1;

  GURL page1("data:text/html,hello world1");
  ui_test_utils::WindowedTabAddedNotificationObserver observer1(
      content::NotificationService::AllSources());
  chrome::ShowSingletonTab(browser(), page1);
  observer1.Wait();
  tab_count++;
  host_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  // DevTools start in docked mode (no new tab), in a separate process.
  chrome::ToggleDevToolsWindow(browser(), DevToolsToggleAction::Inspect());
  host_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  WebContents* devtools = FindFirstDevToolsContents();
  DCHECK(devtools);

  // DevTools start in a separate process.
  DevToolsWindow::OpenDevToolsWindow(devtools, DevToolsToggleAction::Inspect());
  host_count++;
  EXPECT_EQ(tab_count, browser()->tab_strip_model()->count());
  EXPECT_EQ(host_count, RenderProcessHostCount());

  // close docked devtools
  content::WindowedNotificationObserver close_observer(
      content::NOTIFICATION_WEB_CONTENTS_DESTROYED,
      content::Source<content::WebContents>(devtools));
  chrome::ToggleDevToolsWindow(browser(), DevToolsToggleAction::Toggle());
  close_observer.Wait();
}

// This class's goal is to close the browser window when a renderer process has
// crashed. It does so by monitoring WebContents for RenderProcessGone event and
// closing the passed in TabStripModel. This is used in the following test case.
class WindowDestroyer : public content::WebContentsObserver {
 public:
  WindowDestroyer(content::WebContents* web_contents, TabStripModel* model)
      : content::WebContentsObserver(web_contents), tab_strip_model_(model) {}

  void RenderProcessGone(base::TerminationStatus status) override {
    // Wait for the window to be destroyed, which will ensure all other
    // RenderViewHost objects are deleted before we return and proceed with
    // the next iteration of notifications.
    content::WindowedNotificationObserver observer(
        chrome::NOTIFICATION_BROWSER_CLOSED,
        content::NotificationService::AllSources());
    tab_strip_model_->CloseAllTabs();
    observer.Wait();
  }

 private:
  TabStripModel* tab_strip_model_;

  DISALLOW_COPY_AND_ASSIGN(WindowDestroyer);
};

// Test to ensure that while iterating through all listeners in
// RenderProcessHost and invalidating them, we remove them properly and don't
// access already freed objects. See http://crbug.com/255524.
IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostTest,
                       CloseAllTabsDuringProcessDied) {
  GURL url(chrome::kChromeUIOmniboxURL);

  ui_test_utils::NavigateToURL(browser(), url);
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), url, NEW_BACKGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_NAVIGATION);

  EXPECT_EQ(2, browser()->tab_strip_model()->count());

  WebContents* wc1 = browser()->tab_strip_model()->GetWebContentsAt(0);
  WebContents* wc2 = browser()->tab_strip_model()->GetWebContentsAt(1);
  EXPECT_EQ(wc1->GetRenderProcessHost(), wc2->GetRenderProcessHost());

  // Create an object that will close the window on a process crash.
  WindowDestroyer destroyer(wc1, browser()->tab_strip_model());

  content::WindowedNotificationObserver observer(
      chrome::NOTIFICATION_BROWSER_CLOSED,
      content::NotificationService::AllSources());

  // Kill the renderer process, simulating a crash. This should the ProcessDied
  // method to be called. Alternatively, RenderProcessHost::OnChannelError can
  // be called to directly force a call to ProcessDied.
  wc1->GetRenderProcessHost()->Shutdown(-1, true);

  observer.Wait();
}

// Sets up the browser in order to start the tests with two tabs open: one
// called "no audio" in foreground and another called "audio" in background with
// audio in playing state. Also sets up the variables containing the process
// associated with each tab, the urls of the two pages and the WebContents of
// the "audio" page.
class ChromeRenderProcessHostBackgroundingTest
    : public ChromeRenderProcessHostTest {
 public:
  ChromeRenderProcessHostBackgroundingTest() {}

  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitch(switches::kProcessPerTab);
  }

  void SetUpOnMainThread() override {
    ASSERT_TRUE(embedded_test_server()->Start());

    // Set up the server and get the test pages.
    base::FilePath test_data_dir;
    ASSERT_TRUE(PathService::Get(base::DIR_SOURCE_ROOT, &test_data_dir));
    embedded_test_server()->ServeFilesFromDirectory(
        test_data_dir.AppendASCII("chrome/test/data/"));
    audio_url_ = embedded_test_server()->GetURL("/extensions/loop_audio.html");
    no_audio_url_ = embedded_test_server()->GetURL("/title1.html");

    // Open a browser, navigate to the audio page and get its WebContents.
    ui_test_utils::NavigateToURL(browser(), audio_url_);
    audio_tab_web_contents_ =
        browser()->tab_strip_model()->GetActiveWebContents();

    // Create a new tab for the no audio page and confirm that the process of
    // each tab is different and that both are valid.
    audio_process_ = ProcessFromHandle(
        audio_tab_web_contents_->GetRenderProcessHost()->GetHandle());
    no_audio_process_ = ShowSingletonTab(no_audio_url_);
    ASSERT_NE(audio_process_.Pid(), no_audio_process_.Pid());
    ASSERT_TRUE(no_audio_process_.IsValid());
    ASSERT_TRUE(audio_process_.IsValid());
  }

 protected:
  GURL audio_url_;
  GURL no_audio_url_;

  base::Process audio_process_;
  base::Process no_audio_process_;

  content::WebContents* audio_tab_web_contents_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ChromeRenderProcessHostBackgroundingTest);
};

// Test to make sure that a process is backgrounded when the audio stops playing
// from the active tab and there is an immediate tab switch.
IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostBackgroundingTest,
                       ProcessPriorityAfterStoppedAudio) {
  // This test is invalid on platforms that can't background.
  if (!base::Process::CanBackgroundProcesses())
    return;

  ShowSingletonTab(audio_url_);

  // Wait until the no audio page is backgrounded and the audio page is not
  // backgrounded.
  while (!no_audio_process_.IsProcessBackgrounded() ||
         audio_process_.IsProcessBackgrounded()) {
    base::RunLoop().RunUntilIdle();
    base::PlatformThread::Sleep(TestTimeouts::tiny_timeout());
  }

  // Pause the audio and immediately switch to the no audio tab.
  ASSERT_TRUE(content::ExecuteScript(
      audio_tab_web_contents_,
      "document.getElementById('audioPlayer').pause();"));
  ShowSingletonTab(no_audio_url_);

  // Wait until the no audio page is not backgrounded and the audio page is
  // backgrounded.
  while (no_audio_process_.IsProcessBackgrounded() ||
         !audio_process_.IsProcessBackgrounded()) {
    base::RunLoop().RunUntilIdle();
    base::PlatformThread::Sleep(TestTimeouts::tiny_timeout());
  }
}

// Test to make sure that a process is backgrounded automatically when audio
// stops playing from a hidden tab.
IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostBackgroundingTest,
                       ProcessPriorityAfterAudioStopsOnNotVisibleTab) {
  // This test is invalid on platforms that can't background.
  if (!base::Process::CanBackgroundProcesses())
    return;

  // Wait until the two pages are not backgrounded.
  while (no_audio_process_.IsProcessBackgrounded() ||
         audio_process_.IsProcessBackgrounded()) {
    base::RunLoop().RunUntilIdle();
    base::PlatformThread::Sleep(TestTimeouts::tiny_timeout());
  }

  // Stop the audio.
  ASSERT_TRUE(content::ExecuteScript(
      audio_tab_web_contents_,
      "document.getElementById('audioPlayer').pause();"));

  // Wait until the no audio page is not backgrounded and the audio page is
  // backgrounded.
  while (no_audio_process_.IsProcessBackgrounded() ||
         !audio_process_.IsProcessBackgrounded()) {
    base::RunLoop().RunUntilIdle();
    base::PlatformThread::Sleep(TestTimeouts::tiny_timeout());
  }
}

// Test to make sure that a process is un-backgrounded automatically when audio
// starts playing from a backgrounded tab.
IN_PROC_BROWSER_TEST_F(ChromeRenderProcessHostBackgroundingTest,
                       ProcessPriorityAfterAudioStartsFromBackgroundTab) {
  // This test is invalid on platforms that can't background.
  if (!base::Process::CanBackgroundProcesses())
    return;

  // Stop the audio.
  ASSERT_TRUE(content::ExecuteScript(
      audio_tab_web_contents_,
      "document.getElementById('audioPlayer').pause();"));

  // Wait until the no audio page is not backgrounded and the audio page is
  // backgrounded.
  while (no_audio_process_.IsProcessBackgrounded() ||
         !audio_process_.IsProcessBackgrounded()) {
    base::RunLoop().RunUntilIdle();
    base::PlatformThread::Sleep(TestTimeouts::tiny_timeout());
  }

  // Start the audio from the backgrounded tab.
  ASSERT_TRUE(
      content::ExecuteScript(audio_tab_web_contents_,
                             "document.getElementById('audioPlayer').play();"));

  // Wait until the two pages are not backgrounded.
  while (no_audio_process_.IsProcessBackgrounded() ||
         audio_process_.IsProcessBackgrounded()) {
    base::RunLoop().RunUntilIdle();
    base::PlatformThread::Sleep(TestTimeouts::tiny_timeout());
  }
}