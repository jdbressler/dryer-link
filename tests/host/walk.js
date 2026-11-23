// Clicks through the real setup page in a phone-sized browser, against the firmware's own logic (build/sim --serve).
//   node walk.js            needs ./build.sh to have been run, and Playwright with Chromium
// Screenshots go to build/shots/.
const { chromium } = require('playwright');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const SPEED = 10;                      // module time runs this much faster than the clock
const SHOTS = path.join(__dirname, 'build', 'shots');
let port = 8100, failed = 0, checks = 0;

function check(ok, what, detail) { checks++; if (!ok) { failed++; console.log('  FAILED: ' + what + (detail ? '\n     got: ' + detail : '')); } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function withModule(dryer, fn, extra = []) {
  const p = ++port;
  const sim = spawn(path.join(__dirname, 'build', 'sim'), ['--serve', String(p), '--speed', String(SPEED), '--dryer', dryer, '--log', '1', ...extra], { stdio: ['ignore', 'inherit', 'inherit'] });
  await sleep(400);
  const base = 'http://127.0.0.1:' + p;
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 1 });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  const sim_ = async (q) => (await fetch(base + '/sim/' + q)).json();
  const sees = async (text, ms = 8000) => { try { await page.getByText(text, { exact: false }).first().waitFor({ timeout: ms }); return true; } catch (e) { return false; } };
  const expectSees = async (text, ms) => check(await sees(text, ms), 'page shows "' + text + '"', (await page.locator('#app').innerText()).slice(0, 300).replace(/\n+/g, ' | '));
  const tap = (name) => page.getByRole('button', { name }).first().click();
  const shot = (name) => page.screenshot({ path: path.join(SHOTS, dryer + (extra.includes('--test-build') ? '_testbuild' : '') + '_' + name + '.png') });
  try { await fn({ page, base, sim: sim_, sees, expectSees, tap, shot }); } finally {
    check(errors.length === 0, 'no script errors on the page', errors.join(' / '));
    await browser.close(); sim.kill();
  }
}

(async () => {
  fs.mkdirSync(SHOTS, { recursive: true });

  console.log('1. the whole walkthrough on a dryer like the GTD65');
  await withModule('gtd65', async ({ page, base, sim, sees, expectSees, tap, shot }) => {
    await page.goto(base + '/hotspot-detect.html');           // what an iPhone asks for when it joins the hotspot
    await expectSees("Let's connect your dryer");
    check(!(await page.locator('#demo').innerText()).trim(), 'no "preview only" controls on the real module');
    await tap('Get started');
    await expectSees('Found your dryer');
    await expectSees('GTD65GBSJ3WS');
    await expectSees('Clothes Dryer');
    await shot('01_found');
    await tap('Continue');
    await expectSees("Now I'll learn your dryer");
    await tap('Start the quick check');
    await expectSees('Learning your dryer');
    await sleep(2500);
    await shot('02_learning');
    const pct = parseInt(await page.locator('.pct').innerText(), 10);
    check(pct > 3 && pct < 60, 'progress is moving', String(pct));
    check(/of 517/.test(await page.locator('.ring small').innerText()), 'progress counts real codes', await page.locator('.ring small').innerText());
    await expectSees('Got it', 30000);
    await expectSees('What the dryer is doing');
    await expectSees('Extended Tumble');
    await expectSees("6 things this model doesn't have were left out");
    await expectSees("5 codes I don't use yet");
    check(!(await sees('Dryness level', 300)), 'absent things are not listed');
    await shot('03_learned');
    await tap('Continue');
    await expectSees('Open the dryer door');
    await sim('door?open=1');
    await expectSees('Now close it');
    await shot('04_door_open');
    await sim('door?open=0');
    await expectSees('Door check passed');
    await tap('Continue');
    await expectSees('Press Power on the dryer');
    await sim('state?v=1');
    await expectSees('Power check passed');
    await tap('Continue');
    await expectSees('Connect to your Wi-Fi');
    await expectSees('NETGEAR-5G-2241');
    check((await page.locator('#nets li').count()) === 3, 'networks listed once each', String(await page.locator('#nets li').count()));
    await shot('05_wifi');
    await page.locator('#nets li', { hasText: 'Maple House' }).first().click();
    await expectSees('Enter the password for this network');
    await page.locator('#psk').fill('short');
    await tap('Connect');
    check(await sees('Enter the password', 500), 'a password under 8 characters is not sent');
    await page.locator('#psk').fill('wrongpass');
    await tap('Connect');
    await expectSees('Joining your Wi-Fi');
    await expectSees('dryer-link-3fa2c4');
    await expectSees("That didn't work", 12000);
    await shot('06_wrong_password');
    await tap('Try the password again');
    await page.locator('#psk').fill('the right one');
    await tap('Connect');
    // The module joins and closes its setup network, so the page can no longer reach it. That is the normal ending.
    await expectSees('That should be it', 12000);
    await expectSees('the green light on the module is on');
    await expectSees('Sign in as admin with the password on the module');
    await expectSees('http://dryer-link-3fa2c4.local');
    check(!(await sees('Lost the connection', 300)), 'no "lost the connection" warning on a successful join');
    await shot('07_done');
    await sleep(2500);                                        // the module restarts itself a few seconds after joining
    const info = await sim('info');
    check(info.boots === 2, 'the module restarted once after joining Wi-Fi', JSON.stringify(info));
    check(JSON.stringify(info.hidden) === '["0x204D","0x2050","0x2003","0x2047","0x206F","0x2053"]', 'exactly the absent features are hidden', JSON.stringify(info.hidden));
    check(info.writes === 1 && info.faults === 0, 'one write (the Extended Tumble check), no faults', JSON.stringify(info));
    check(info.hotspot === false, 'hotspot is gone');
    // The release build serves nothing of its own on the home network.
    for (const p of ['/setup', '/dl/status', '/dl/report', '/dl/learn?mode=full'])
      check((await fetch(base + p)).status === 404, 'not served on the home network: ' + p);
  }, ['--label', 'kq7m-x2hd-9pwc']);

  console.log('1b. the same on a test build (walkthrough also on the home network, no label)');
  await withModule('gtd65', async ({ page, base, sim, sees, expectSees, tap, shot }) => {
    await page.goto(base + '/');
    await tap('Get started');
    await expectSees('Found your dryer');
    await tap('Continue');
    await tap('Start the quick check');
    await expectSees('Got it', 30000);
    await tap('Continue');
    await tap('Skip this check');
    await expectSees('Press Power on the dryer');
    await tap('Skip this check');
    await expectSees('Connect to your Wi-Fi');
    await page.locator('#nets li', { hasText: 'Maple House' }).first().click();
    await page.locator('#psk').fill('the right one');
    await tap('Connect');
    await expectSees("You're all set", 12000);                // in this pretend set-up the page can still reach the module
    check(!(await sees('Sign in as', 300)), 'no sign-in hint when the status page has no login');
    await sleep(2500);
    const info = await sim('info');
    check(info.boots === 2 && info.hotspot === false, 'restarted once, hotspot gone', JSON.stringify(info));
    const report = await (await fetch(base + '/dl/report')).text();
    check(/model: GTD65GBSJ3WS/.test(report) && /learned: yes, quick check/.test(report), 'report is there after the restart', report.slice(0, 200));
    // Opening the page again later lands on the result, not on the welcome screen.
    await page.goto(base + '/setup');
    await expectSees('Got it');
  }, ['--test-build']);

  console.log('2. no dryer on the cable, then it is plugged in');
  await withModule('none', async ({ page, base, sim, sees, expectSees, tap, shot }) => {
    await page.goto(base + '/');
    await tap('Get started');
    await expectSees("I can't hear the dryer yet", 12000);
    await shot('01_not_found');
    await page.locator('#help').click();
    await expectSees('Connect the module to Wi-Fi anyway');
    await page.getByRole('button', { name: 'Copy the report' }).click();
    await sleep(600);
    await shot('02_help');
    await tap('Close');
    await sim('cable?in=1');
    await expectSees('Found your dryer', 12000);               // without tapping anything
    await tap('Continue');
    await tap('Start the quick check');
    await expectSees('Learning your dryer');
    await sleep(1500);
    await page.reload();                                      // the phone closed the page; it is opened again
    await expectSees('Learning your dryer');
    check(parseInt(await page.locator('.pct').innerText(), 10) > 5, 'a reopened page shows the check already running');
    await sim('cable?in=0');                                  // and now the cable falls out
    await expectSees("I can't hear the dryer yet", 20000);
    const st = await (await fetch(base + '/dl/status')).json();
    check(st.learn.state === 'idle', 'the interrupted check left nothing behind', JSON.stringify(st.learn));
    // Help offers a way onto Wi-Fi anyway.
    await page.locator('#help').click();
    await tap('Connect the module to Wi-Fi anyway');
    await expectSees('Connect to your Wi-Fi');
  });

  console.log('3. a washer');
  await withModule('washer', async ({ page, base, expectSees, tap, shot }) => {
    await page.goto(base + '/');
    await tap('Get started');
    await expectSees("This isn't a dryer I know");
    await expectSees('Clothes Washer');
    await shot('01_washer');
  });

  console.log('4. a dryer that speaks the new language');
  await withModule('new', async ({ page, base, sim, sees, expectSees, tap, shot }) => {
    await page.goto(base + '/');
    await tap('Get started');
    await expectSees('Found your dryer', 12000);
    await expectSees('PTD60GBSRWS');
    await tap('Continue');
    await tap('Do a full scan instead');
    await expectSees('about 12 minutes');
    await shot('01_full_sheet');
    await tap('Start the full scan');
    await expectSees('Got it', 30000);
    check(!(await sees('Extended Tumble', 300)), 'a control this dryer lacks is not offered');
    await tap('Continue');
    await expectSees('Open the dryer door');
    await tap('Skip this check');
    await expectSees('Press Power on the dryer');
    await tap('Skip this check');
    await expectSees('Connect to your Wi-Fi');
  });

  console.log(checks + ' checks, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})();
