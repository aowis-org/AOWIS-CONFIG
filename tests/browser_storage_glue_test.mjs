// Executes the EM_JS bodies of src/browser_backend.cpp under Node with fake
// browser APIs (localStorage, navigator.locks). This covers the JavaScript
// half of the browser backend without a browser or an Emscripten toolchain;
// the C++ half is covered by the backend contract tests on a fake store.
//
// Usage: node browser_storage_glue_test.mjs <path to browser_backend.cpp>
import { readFileSync } from "node:fs";
import vm from "node:vm";
import assert from "node:assert/strict";

const source = readFileSync(process.argv[2], "utf8");

// Extracts EM_JS(type, name, (params), { body }) blocks by brace matching.
function extract(text) {
    const functions = {};
    const pattern = /EM_JS\(\s*[^,]+,\s*(\w+),\s*\(([^)]*)\),\s*\{/g;
    let match;
    while ((match = pattern.exec(text)) !== null) {
        let depth = 1;
        let index = pattern.lastIndex;
        let quote = null;
        for (; index < text.length && depth > 0; ++index) {
            const c = text[index];
            if (quote) {
                if (c === "\\") ++index;
                else if (c === quote) quote = null;
            } else if (c === '"' || c === "'" || c === "`") quote = c;
            else if (c === "{") ++depth;
            else if (c === "}") --depth;
        }
        const body = text.slice(pattern.lastIndex, index - 1);
        const params = match[2].split(",").map((p) => p.trim()).filter(Boolean)
            .map((p) => p.split(/[\s*]+/).pop());
        functions[match[1]] = { params, body };
    }
    return functions;
}

const glue = extract(source);
for (const name of ["aowis_config_ls_get", "aowis_config_ls_put", "aowis_config_ls_remove",
                    "aowis_config_ls_keys", "aowis_config_lease"]) {
    assert.ok(glue[name], `missing EM_JS function ${name}`);
}

class FakeStorage {
    constructor(quota = Infinity) { this.map = new Map(); this.quota = quota; }
    get length() { return this.map.size; }
    key(index) { return [...this.map.keys()][index] ?? null; }
    getItem(key) { return this.map.has(key) ? this.map.get(key) : null; }
    setItem(key, value) {
        if (String(value).length > this.quota) {
            const error = new Error("quota"); error.name = "QuotaExceededError"; throw error;
        }
        this.map.set(key, String(value));
    }
    removeItem(key) { this.map.delete(key); }
}

// Exclusive locks shared between "tabs" (contexts); callbacks are granted
// asynchronously, like the real Web Locks API.
class FakeLockManager {
    constructor() { this.held = new Set(); this.queue = []; }
    request(name, options, callback) {
        return new Promise((resolve, reject) => {
            this.queue.push({ name, callback, resolve, reject });
            setTimeout(() => this.grant(), 0);
        });
    }
    grant() {
        for (const waiter of [...this.queue]) {
            if (this.held.has(waiter.name)) continue;
            this.queue.splice(this.queue.indexOf(waiter), 1);
            this.held.add(waiter.name);
            Promise.resolve(waiter.callback()).then(() => { this.held.delete(waiter.name); this.grant(); });
        }
    }
    release(name) { this.held.delete(name); this.grant(); }
}

// One browser tab: its own globals, shared storage/locks of the origin.
function tab(storage, locks) {
    const context = vm.createContext({
        UTF8ToString: (value) => value,       // Tests pass JS strings directly.
        stringToNewUTF8: (value) => value,    // ...and read returned strings directly.
        Promise, setTimeout,
    });
    if (storage !== undefined) {
        Object.defineProperty(context, "localStorage", { get: () => {
            if (storage === "throws") throw new Error("SecurityError");
            return storage;
        } });
    }
    if (locks !== undefined) context.navigator = { locks };
    const api = {};
    for (const [name, { params, body }] of Object.entries(glue)) {
        api[name] = vm.runInContext(`(function (${params.join(", ")}) {${body}})`, context);
    }
    return api;
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 5));

// --- localStorage accessors ---
{
    const storage = new FakeStorage();
    const page = tab(storage, new FakeLockManager());
    assert.equal(page.aowis_config_ls_get("k"), 0, "missing key must be null");
    assert.equal(page.aowis_config_ls_put("k", "{\"a\":1}", 1), 0);
    assert.equal(page.aowis_config_ls_get("k"), "v{\"a\":1}");
    assert.equal(page.aowis_config_ls_put("k", "{}", 1), 1, "create_only must not replace");
    assert.equal(page.aowis_config_ls_get("k"), "v{\"a\":1}");
    assert.equal(page.aowis_config_ls_put("k", "{}", 0), 0, "replace");
    assert.equal(page.aowis_config_ls_put("", "", 0), 0, "empty value stored");
    assert.equal(page.aowis_config_ls_get(""), "v", "empty value distinct from missing");
    assert.equal(page.aowis_config_ls_remove("k"), 0);
    assert.equal(page.aowis_config_ls_remove("k"), 4, "removing a missing key");
    storage.setItem("aowis-config:/aowis/gui/items/a.json", "1");
    storage.setItem("aowis-config:/aowis/gui/items/b.json", "2");
    storage.setItem("aowis-config:/aowis/gui-mobile/items/c.json", "3");
    storage.setItem("aowis-config:/aowis/gui/items/bad\u001f.json", "4");
    storage.setItem("other-app", "5");
    const keys = page.aowis_config_ls_keys("aowis-config:/aowis/gui/").slice(1).split("\u001f").sort();
    assert.deepEqual(keys, ["aowis-config:/aowis/gui/items/a.json", "aowis-config:/aowis/gui/items/b.json"]);
    assert.equal(page.aowis_config_ls_keys("nothing-here"), "v", "no keys");
}
{
    const page = tab(new FakeStorage(4), new FakeLockManager());
    assert.equal(page.aowis_config_ls_put("k", "too long", 0), 2, "quota must be reported");
}
{
    const page = tab("throws", new FakeLockManager());
    assert.equal(page.aowis_config_ls_get("k"), "eunavailable");
    assert.equal(page.aowis_config_ls_put("k", "v", 0), 3);
    assert.equal(page.aowis_config_ls_remove("k"), 3);
    assert.equal(page.aowis_config_ls_keys("k"), "eunavailable");
}

// --- Web Locks writer lease ---
{
    const storage = new FakeStorage();
    const locks = new FakeLockManager();
    const first = tab(storage, locks);
    const second = tab(storage, locks);
    assert.equal(first.aowis_config_lease("/aowis/gui"), 1, "lease is pending until granted");
    await tick();
    assert.equal(first.aowis_config_lease("/aowis/gui"), 0, "first tab becomes the writer");
    assert.equal(second.aowis_config_lease("/aowis/gui"), 1, "second tab waits");
    await tick();
    assert.equal(second.aowis_config_lease("/aowis/gui"), 1, "lease is held for the first tab's lifetime");
    assert.equal(second.aowis_config_lease("/aowis/gui-mobile"), 1);
    await tick();
    assert.equal(second.aowis_config_lease("/aowis/gui-mobile"), 0, "other owner roots are independent");
    locks.release("aowis-config:/aowis/gui"); // First tab closed.
    await tick();
    assert.equal(second.aowis_config_lease("/aowis/gui"), 0, "waiting tab takes over automatically");
}
{
    const page = tab(new FakeStorage(), undefined);
    assert.equal(page.aowis_config_lease("/aowis/gui"), 2, "no Web Locks: unavailable (fail closed)");
}
{
    const page = tab(new FakeStorage(), { request: () => Promise.reject(new Error("denied")) });
    assert.equal(page.aowis_config_lease("/aowis/gui"), 1);
    await tick();
    assert.equal(page.aowis_config_lease("/aowis/gui"), 2, "rejected lock request: unavailable");
}

console.log("AOWIS-CONFIG browser storage glue tests passed");
