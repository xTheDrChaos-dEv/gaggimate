import './style.css';
import { initializeTheme, setMachineBackground, setMachineTheme } from './utils/themeManager.js';
import { webBackgroundForStyle, webThemeForDisplayTheme } from './utils/people.js';
import { Backdrop } from './components/Backdrop.jsx';
import { I18nProvider } from '@lingui/react';
import { activateLocale, i18n } from './i18n.js';
import { isApp } from './app/platform.js';
import { hasConnected, initApp, onAppRendered } from './app/index.js';
import { ConnectionGate } from './app/ConnectionGate.jsx';
import { activeMachine, showMachineSetup } from './app/machines.js';
import { MachineSetup } from './app/MachineSetup.jsx';
import { checkSetup, setupNeeded } from './pages/SetupWizard/setupState.js';

if (import.meta.env.DEV) {
  // Dev-only Preact warnings; stripped from production builds.
  import('preact/debug');
}

import { render } from 'preact';
import { useEffect, useState } from 'preact/hooks';
import { LocationProvider, Router, Route, ErrorBoundary, useLocation } from 'preact-iso';
import isoLazy from 'preact-iso/lazy';

import ApiService, { ApiServiceContext, machine } from './services/ApiService.js';
import { effect } from '@preact/signals';
import { Navigation } from './components/Navigation.jsx';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { faBars } from '@fortawesome/free-solid-svg-icons/faBars';

// A page part that will not load nearly always means the machine was updated while this page stayed open: the
// new firmware has the same pages under new file names. A home-screen web app is the usual victim, because the
// phone keeps it alive in the background for days, and it then showed the menu with an empty page next to it.
// Loading the page again fetches the current files; if that just happened and it still fails, say so instead.
const STALE_RELOAD_KEY = 'gaggimate-stale-reload';
function PageLoadFailed() {
  return (
    <div className='bg-base-200 mx-auto mt-8 max-w-md rounded-lg p-6 text-center'>
      <p className='mb-4'>This page could not be loaded from the machine.</p>
      <button className='btn btn-primary' onClick={() => window.location.reload()}>
        Reload
      </button>
    </div>
  );
}
function recoverFromStalePage() {
  let reloadedAt = 0;
  try {
    reloadedAt = Number(window.sessionStorage.getItem(STALE_RELOAD_KEY)) || 0;
    if (Date.now() - reloadedAt > 20000) {
      window.sessionStorage.setItem(STALE_RELOAD_KEY, String(Date.now()));
    }
  } catch {
    reloadedAt = Date.now(); // no storage to guard against a reload loop, so do not start one
  }
  if (Date.now() - reloadedAt > 20000) {
    window.location.reload();
    return new Promise(() => {}); // the page is going away; show nothing in the meantime
  }
  return PageLoadFailed;
}
const lazy = load => isoLazy(() => load().catch(recoverFromStalePage));

// Each page lazy-loads as its own Vite chunk so the initial bundle stays small.
// Chart.js, FontAwesome icon sets, and the analyzer/statistics views are too
// large to ship up-front on the ESP32's slow WiFi pipe.
const Home = lazy(() => import('./pages/Home/index.jsx').then(m => m.Home));
const NotFound = lazy(() => import('./pages/_404.jsx').then(m => m.NotFound));
const Settings = lazy(() => import('./pages/Settings/index.jsx').then(m => m.Settings));
const ProfileList = lazy(() => import('./pages/ProfileList/index.jsx').then(m => m.ProfileList));
const ProfileEdit = lazy(() => import('./pages/ProfileEdit/index.jsx').then(m => m.ProfileEdit));
const ShotHistory = lazy(() => import('./pages/ShotHistory/index.jsx').then(m => m.ShotHistory));
const ShotAnalyzer = lazy(() => import('./pages/ShotAnalyzer/index.jsx').then(m => m.ShotAnalyzer));
const StatisticsPage = lazy(() =>
  import('./pages/Statistics/index.jsx').then(m => m.StatisticsPage),
);
// PURE lets the firmware build drop this app-only page entirely.
const AppSettings = /* @__PURE__ */ lazy(() =>
  import('./app/AppSettings.jsx').then(m => m.AppSettings),
);
const SetupWizard = lazy(() => import('./pages/SetupWizard/index.jsx').then(m => m.SetupWizard));
const DashboardSettings = lazy(() =>
  import('./pages/DashboardSettings/index.jsx').then(m => m.DashboardSettings),
);

// Created in boot() so the app knows which machine to connect to first.
let apiService;
const DESKTOP_NAV_COLLAPSED_STORAGE_KEY = 'gaggimate.desktopNavCollapsed';

function readInitialDesktopNavCollapsed() {
  // The drawer is an overlay below md, so it always starts closed there; the stored state is desktop-only.
  if (globalThis.window?.innerWidth < 768) return true;
  const storage = globalThis.window?.localStorage;
  if (!storage) return true;

  try {
    return storage.getItem(DESKTOP_NAV_COLLAPSED_STORAGE_KEY) !== 'false';
  } catch {
    return true;
  }
}

const RedirectTo = to =>
  function Redirect() {
    const loc = useLocation();
    useEffect(() => {
      loc.route(to, true);
    }, [loc]);
    return null;
  };

export function App() {
  const [navCollapsed, setNavCollapsed] = useState(readInitialDesktopNavCollapsed);

  useEffect(() => {
    const storage = globalThis.window?.localStorage;
    if (!storage || globalThis.window.innerWidth < 768) return;

    try {
      storage.setItem(DESKTOP_NAV_COLLAPSED_STORAGE_KEY, String(navCollapsed));
    } catch {
      // Ignore storage write failures so the navigation still works in restricted browsers.
    }
  }, [navCollapsed]);

  useEffect(() => {
    const handleOpenNav = () => setNavCollapsed(false);
    window.addEventListener('open-mobile-nav', handleOpenNav);
    return () => window.removeEventListener('open-mobile-nav', handleOpenNav);
  }, []);

  const machineKey = activeMachine.value?.id;
  useEffect(() => {
    checkSetup({ fresh: isApp });
  }, [machineKey]);

  if (isApp && !activeMachine.value) return <MachineSetup mandatory />;
  if (isApp && !hasConnected.value) {
    return showMachineSetup.value ? <MachineSetup /> : <ConnectionGate />;
  }

  return (
    <LocationProvider>
      <ApiServiceContext.Provider value={apiService}>
        {isApp && showMachineSetup.value && <MachineSetup />}
        <div className='bg-base-300 relative isolate flex h-screen overflow-hidden pt-[env(safe-area-inset-top)] pr-[env(safe-area-inset-right)] pb-[env(safe-area-inset-bottom)] pl-[env(safe-area-inset-left)]'>
          <Backdrop />
          <Navigation
            collapsed={navCollapsed}
            onToggleCollapsed={() => setNavCollapsed(collapsed => !collapsed)}
          />
          <div className='flex flex-1 flex-col overflow-x-hidden overflow-y-auto'>
            <div className='flex min-h-0 w-full flex-1 flex-col p-4'>
              <div className='grid min-h-0 flex-1 grid-cols-1'>
                <div className='min-h-0'>
                  <ErrorBoundary>
                    {setupNeeded.value ? (
                      <SetupWizard />
                    ) : (
                      <Router>
                        <Route path='/' component={Home} />
                        <Route path='/setup' component={SetupWizard} />
                        <Route path='/dashboard-settings' component={DashboardSettings} />
                        <Route path='/profiles' component={ProfileList} />
                        <Route path='/profiles/:id' component={ProfileEdit} />
                        <Route path='/settings/:tab?' component={Settings} />
                        {isApp && <Route path='/app-settings' component={AppSettings} />}
                        {/* Legacy routes now live in settings tabs */}
                        <Route path='/ota' component={RedirectTo('/settings/system')} />
                        <Route path='/scales' component={RedirectTo('/settings/bluetooth')} />
                        <Route path='/pidtune' component={RedirectTo('/settings/calibration')} />
                        <Route path='/history' component={ShotHistory} />
                        <Route path='/analyzer' component={ShotAnalyzer} />
                        <Route path='/statistics' component={StatisticsPage} />
                        <Route
                          path='/statistics/:sourceAlias/:profileName'
                          component={StatisticsPage}
                        />
                        {/* deep-link route (source and id) */}
                        <Route path='/analyzer/:source/:id' component={ShotAnalyzer} />
                        <Route default component={NotFound} />
                      </Router>
                    )}
                  </ErrorBoundary>
                </div>
              </div>
            </div>
          </div>
          {navCollapsed && (
            <div className='fab end-auto bottom-[max(1rem,env(safe-area-inset-bottom))] left-[max(1rem,env(safe-area-inset-left))] md:hidden landscape:hidden'>
              <button
                className='btn btn-lg btn-circle btn-primary'
                onClick={() => setNavCollapsed(false)}
              >
                <FontAwesomeIcon icon={faBars} />
              </button>
            </div>
          )}
        </div>
      </ApiServiceContext.Provider>
    </LocationProvider>
  );
}

async function boot() {
  if (isApp) await initApp();
  apiService = new ApiService();
  apiService.connect();
  // Must be called before render
  initializeTheme();
  // The "match the machine" web theme follows the display theme of whoever is brewing.
  effect(() => {
    const displayTheme = machine.value.status.displayTheme;
    if (Number.isInteger(displayTheme)) setMachineTheme(webThemeForDisplayTheme(displayTheme));
    // The pages get the same background as the round screen, whatever web theme is selected.
    const backgroundStyle = machine.value.status.backgroundStyle;
    if (Number.isInteger(backgroundStyle)) setMachineBackground(webBackgroundForStyle(backgroundStyle));
  });
  await activateLocale();
  render(
    <I18nProvider i18n={i18n}>
      <App />
    </I18nProvider>,
    document.getElementById('app'),
  );
  if (isApp) onAppRendered(apiService);
}

boot();
