#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 KAIOS25.zip KAIOS3.zip" >&2
  exit 2
fi
kaios25=$(realpath -m "$1")
kaios3=$(realpath -m "$2")
staging=$(mktemp -d "${TMPDIR:-/tmp}/oos-kaios-test.XXXXXX")
trap 'rm -rf "$staging"' EXIT

mkdir -p "$staging/kaios25" "$staging/kaios3"
printf '%s\n' \
  '{"name":"APN Config","version":"1.0.0","launch_path":"/index.html","permissions":{"alarms":{},"device-storage:sdcard":{"access":"readwrite"},"settings":{"access":"readwrite"},"wifi-manage":{}},"messages":[{"alarm":"/index.html"}],"activities":{"pick":{"filters":{"type":["image/*"]}}},"datastores-owned":{"test-state":{"access":"readwrite","description":"WPE bridge test"},"owner-write":{"access":"readonly","description":"Owner remains writable"}}}' \
  > "$staging/kaios25/manifest.webapp"
cat > "$staging/kaios25/index.html" <<'EOF'
<!doctype html><meta charset="utf-8"><title>KaiOS 2.5 API test</title>
<body>RUNNING<script>
(async () => {
  try {
    if (globalThis.__oosRuntime?.apiProfile !== 'kaios-v2' ||
        location.protocol !== 'app:' ||
        location.hostname !== 'org.kaios.apnconfig' ||
        navigator.b2g || !navigator.mozApps || !navigator.mozWifiManager ||
        typeof navigator.getDeviceStorage !== 'function' ||
        typeof navigator.getDataStores !== 'function' ||
        typeof navigator.getFeature !== 'function')
      throw new Error('invalid KaiOS 2.5 API profile');
    const manifest = await fetch('/manifest.webapp').then(response =>
      response.json());
    if (manifest.name !== 'APN Config')
      throw new Error('KaiOS 2 packaged resource fetch failed');
    const manifestHead = await fetch('/manifest.webapp', { method: 'HEAD' });
    if (!manifestHead.ok || !manifestHead.headers.get('content-length'))
      throw new Error('KaiOS 2 packaged resource HEAD failed');
    try {
      await navigator.mozWifiManager.refresh();
      throw new Error('Wi-Fi control unexpectedly succeeded');
    } catch (error) {
      if (error.name !== 'NotSupportedError') throw error;
    }
    try {
      navigator.mozWifiManager.enabled = true;
      throw new Error('Wi-Fi enabled setter unexpectedly succeeded');
    } catch (error) {
      if (error.name !== 'NotSupportedError') throw error;
    }
    const settings = navigator.mozSettings.createLock();
    await new Promise((resolve, reject) => {
      const request = settings.set({ 'oos.test': 42 });
      request.onsuccess = resolve; request.onerror = () => reject(request.error);
    });
    const alarmId = await new Promise((resolve, reject) => {
      const request = navigator.mozAlarms.add(new Date(Date.now() + 60000),
        'honorTimezone', { test: true });
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
    navigator.mozAlarms.remove(alarmId);
    const [store] = await navigator.getDataStores('test-state');
    await store.clear();
    const id = await store.add({ value: 7 });
    if ((await store.get(id))?.value !== 7 || await store.getLength() !== 1)
      throw new Error('DataStore host bridge failed');
    await store.put({ value: 9 }, id, store.revisionId);
    const cursor = store.sync();
    if ((await cursor.next()).operation !== 'add')
      throw new Error('DataStore sync cursor failed');
    if (!await store.remove(id, store.revisionId) ||
        await store.getLength() !== 0)
      throw new Error('DataStore remove/revision bridge failed');
    const [ownerStore] = await navigator.getDataStores('owner-write');
    if (ownerStore.readOnly)
      throw new Error('DataStore owner write access failed');
    await ownerStore.clear();
    const ownerId = await ownerStore.add({ owner: true });
    if (!(await ownerStore.get(ownerId))?.owner)
      throw new Error('DataStore readonly declaration semantics failed');
    document.body.textContent = 'PASS';
    console.log(`OOS_KAIOS_ORIGIN_CHECK result=pass profile=kaios-v2 origin=${location.origin}`);
    await new Promise(resolve => requestAnimationFrame(() =>
      requestAnimationFrame(resolve)));
    setTimeout(() => window.close(), 100);
  } catch (error) {
    document.body.textContent = `FAIL: ${error}`;
    console.error('OOS KaiOS 2.5 API test failed', error);
  }
})();
</script>
EOF
printf '%s\n' \
  '{"name":"Calculator","start_url":"./main.html?source=test","b2g_features":{"version":"3.0.0","permissions":{"alarms":{},"bluetooth":{},"camera":{},"settings":{"access":"readwrite"}},"messages":["alarm"],"activities":{"view":{"filters":{"type":["text/*"]}}}}}' \
  > "$staging/kaios3/manifest.webmanifest"
cat > "$staging/kaios3/main.html" <<'EOF'
<!doctype html><meta charset="utf-8"><title>KaiOS 3 API test</title>
<body>RUNNING<script>
(async () => {
  try {
    if (globalThis.__oosRuntime?.apiProfile !== 'kaios-v3' ||
        location.protocol !== 'http:' || location.port ||
        location.hostname !== 'org.kaios.calculator.localhost' ||
        !navigator.b2g || navigator.mozApps ||
        typeof navigator.b2g.cameras?.refresh !== 'function' ||
        typeof globalThis.lib_devicecapability?.DeviceCapabilityManager?.get !==
          'function' ||
        typeof navigator.getDeviceStorage === 'function')
      throw new Error('invalid KaiOS 3 API profile');
    const manifest = await fetch('/manifest.webmanifest').then(response =>
      response.json());
    if (manifest.name !== 'Calculator')
      throw new Error('KaiOS 3 packaged resource fetch failed');
    const manifestHead = await fetch('/manifest.webmanifest', { method: 'HEAD' });
    if (!manifestHead.ok || !manifestHead.headers.get('content-length'))
      throw new Error('KaiOS 3 packaged resource HEAD failed');
    const syncManifest = new XMLHttpRequest();
    syncManifest.open('GET', '/manifest.webmanifest', false);
    syncManifest.send();
    if (syncManifest.status !== 200 ||
        JSON.parse(syncManifest.responseText).name !== 'Calculator')
      throw new Error('KaiOS 3 packaged resource sync XHR failed');
    const capability = await lib_devicecapability.DeviceCapabilityManager
      .get(new lib_session.Session());
    if (await capability.get('camera-capture') !== 'validated')
      throw new Error('device capability host bridge failed');
    const cameras = await navigator.b2g.cameras.refresh();
    if (cameras[0]?.id !== 'mock-camera-0')
      throw new Error('camera host bridge failed');
    await new Promise((resolve, reject) => {
      const request = navigator.b2g.bluetooth.defaultAdapter.enable();
      request.onsuccess = () => reject(
        new Error('Bluetooth control unexpectedly succeeded'));
      request.onerror = () => request.error?.name === 'NotSupportedError'
        ? resolve() : reject(request.error);
    });
    const alarmId = await navigator.b2g.alarmManager.add({
      date: new Date(Date.now() + 60000), data: { test: true },
      ignoreTimezone: false
    });
    await navigator.b2g.alarmManager.remove(alarmId);
    const settings = await lib_settings.SettingsManager
      .get(new lib_session.Session());
    await settings.set([{ name: 'oos.test', value: 3 }]);
    document.body.textContent = 'PASS';
    console.log(`OOS_KAIOS_ORIGIN_CHECK result=pass profile=kaios-v3 origin=${location.origin}`);
    await new Promise(resolve => requestAnimationFrame(() =>
      requestAnimationFrame(resolve)));
    setTimeout(() => window.close(), 100);
  } catch (error) {
    document.body.textContent = `FAIL: ${error}`;
    console.error('OOS KaiOS 3 API test failed', error);
  }
})();
</script>
EOF

mkdir -p "$(dirname "$kaios25")" "$(dirname "$kaios3")"
(cd "$staging/kaios25" && zip -q -D -9 "$kaios25" manifest.webapp index.html)
(cd "$staging/kaios3" && zip -q -D -9 "$kaios3" manifest.webmanifest main.html)
