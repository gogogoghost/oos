import { chromium } from "playwright-core";

const baseUrl = process.env.LAUNCHER_URL ?? "http://127.0.0.1:4173/";
const browser = await chromium.launch({
  executablePath: process.env.CHROME_BIN ?? "/usr/bin/google-chrome",
  headless: true,
  args: ["--no-sandbox", "--disable-gpu"],
});

try {
  const page = await browser.newPage({
    viewport: { width: 240, height: 320 },
    deviceScaleFactor: 1,
  });
  await page.goto(baseUrl, { waitUntil: "networkidle" });

  const homeLayout = await page.evaluate(() => ({
    width: document.documentElement.scrollWidth,
    height: document.documentElement.scrollHeight,
    status: document.querySelector(".status-bar")?.getBoundingClientRect().toJSON(),
    content: document.querySelector(".screen-content")?.getBoundingClientRect().toJSON(),
    softkeys: document.querySelector(".softkey-bar")?.getBoundingClientRect().toJSON(),
  }));
  if (homeLayout.width !== 240 || homeLayout.height !== 320) {
    throw new Error(`launcher overflowed viewport: ${homeLayout.width}x${homeLayout.height}`);
  }
  if (!homeLayout.status || !homeLayout.content || !homeLayout.softkeys) {
    throw new Error("launcher shell regions are missing");
  }
  if (homeLayout.status.bottom > homeLayout.content.top ||
      homeLayout.content.bottom > homeLayout.softkeys.top) {
    throw new Error("launcher shell regions overlap");
  }
  await page.screenshot({ path: "/tmp/oos-launcher-home.png" });

  await page.keyboard.press("Enter");
  await page.waitForSelector(".app-tile.selected");
  const appLayout = await page.locator(".app-tile").evaluateAll((tiles) =>
    tiles.map((tile) => tile.getBoundingClientRect().toJSON()),
  );
  if (appLayout.length !== 6) {
    throw new Error(`expected 6 app tiles, found ${appLayout.length}`);
  }
  const softkeyTop = homeLayout.softkeys.top;
  for (const tile of appLayout) {
    if (tile.left < 0 || tile.top < homeLayout.content.top || tile.right > 240 ||
        tile.bottom > softkeyTop) {
      throw new Error(`app tile escaped content area: ${JSON.stringify(tile)}`);
    }
  }
  await page.screenshot({ path: "/tmp/oos-launcher-apps.png" });
  console.log("visual_test=ok viewport=240x320 app_tiles=6");
} finally {
  await browser.close();
}
