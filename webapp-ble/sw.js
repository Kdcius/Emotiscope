// Offline cache for the Emotiscope BLE remote.
//
// Cache-first: once installed, the app loads with no network at all -
// only Bluetooth is needed to control the device. Bump CACHE_NAME when
// shipping changes so installed phones pick up the new version.

const CACHE_NAME = "emotiscope-ble-v5";

const PRECACHE = [
	"./",
	"./index.html",
	"./css/app.css",
	"./font/chakra_petch.woff2",
	"./manifest.json",
	"./img/android/android-launchericon-192-192.png",
	"./img/android/android-launchericon-512-512.png",
	"./js/touch_calibration.js",
	"./js/info.js",
	"./js/alerts.js",
	"./js/utilities.js",
	"./js/sliders.js",
	"./js/toggles.js",
	"./js/menu_toggles.js",
	"./js/render_controls.js",
	"./js/ble_connection.js",
	"./js/pages.js",
	"./js/spin_ui.js",
	"./svg/close.svg",
	"./svg/emotiscope.svg",
	"./svg/expand.svg",
	"./svg/info.svg",
	"./svg/lixielabs.svg",
	"./svg/menu.svg",
	"./svg/wand.svg",
];

self.addEventListener("install", (event) => {
	event.waitUntil(
		caches.open(CACHE_NAME)
			// cache: "reload" bypasses the browser's HTTP cache (GitHub Pages
			// serves max-age=600), so a new cache version always precaches
			// fresh copies instead of resurrecting stale ones
			.then((cache) => cache.addAll(PRECACHE.map((url) => new Request(url, { cache: "reload" }))))
			.then(() => self.skipWaiting())
	);
});

self.addEventListener("activate", (event) => {
	event.waitUntil(
		caches.keys()
			.then((keys) => Promise.all(
				keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key))
			))
			.then(() => self.clients.claim())
	);
});

self.addEventListener("fetch", (event) => {
	if (event.request.method !== "GET") { return; }

	// Cache-first with ignoreSearch: the app appends "?v=..." to script URLs
	event.respondWith(
		caches.match(event.request, { ignoreSearch: true }).then((cached) => {
			if (cached) { return cached; }
			return fetch(event.request).then((response) => {
				if (response.ok) {
					const copy = response.clone();
					caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
				}
				return response;
			});
		})
	);
});
