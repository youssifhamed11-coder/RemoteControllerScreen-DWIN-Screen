package com.borg.nnscreen;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Screen Control - Android app for the Neural Nexus DWIN screen.
 * UI = assets/app.html. All HTTP to the ESP32 (http://192.168.4.1) is done here in
 * native code, forced over the Wi-Fi network (the ESP32 Wi-Fi has no internet, so
 * Android would otherwise send the requests over mobile data).
 */
public class MainActivity extends Activity {

    static final String ESP = "http://192.168.4.1";

    private WebView web;
    private ConnectivityManager cm;
    private WifiManager wm;
    private ConnectivityManager.NetworkCallback wifiCallback, espCallback;
    private volatile Network espNet;          // network that last reached the ESP32
    private String pendingSsid, pendingPass;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);

        web = new WebView(this);
        setContentView(web);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);
        web.setWebViewClient(new WebViewClient());
        web.setWebChromeClient(new WebChromeClient());   // alert/confirm dialogs
        web.addJavascriptInterface(new Bridge(), "Android");
        web.loadUrl("file:///android_asset/app.html");

        watchWifi();
    }

    /** Keep the process bound to Wi-Fi whenever a Wi-Fi network is up. */
    private void watchWifi() {
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        wifiCallback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network n) { cm.bindProcessToNetwork(n); }
            @Override public void onLost(Network n) {
                if (n.equals(espNet)) espNet = null;
                cm.bindProcessToNetwork(null);
            }
        };
        try { cm.registerNetworkCallback(req, wifiCallback); } catch (Exception ignored) { }
    }

    /** Networks to try, best first: the one that worked last, then every Wi-Fi network, then default. */
    private List<Network> candidateNetworks() {
        List<Network> list = new ArrayList<>();
        if (espNet != null) list.add(espNet);
        for (Network n : cm.getAllNetworks()) {
            NetworkCapabilities c = cm.getNetworkCapabilities(n);
            if (c != null && c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) && !list.contains(n)) list.add(n);
        }
        list.add(null);   // null = default routing
        return list;
    }

    /** Do one HTTP request to the ESP32. Returns {code, body}; throws if no network can reach it. */
    private String[] http(String method, String path, String body, int timeoutMs) throws Exception {
        try { return httpOnce(method, path, body, timeoutMs); }
        catch (Exception e) { Thread.sleep(400); return httpOnce(method, path, body, timeoutMs); }
    }

    private String[] httpOnce(String method, String path, String body, int timeoutMs) throws Exception {
        Exception last = null;
        for (Network n : candidateNetworks()) {
            HttpURLConnection c = null;
            try {
                URL u = new URL(ESP + path);
                c = (HttpURLConnection) (n != null ? n.openConnection(u) : u.openConnection());
                c.setConnectTimeout(Math.min(timeoutMs, 4000));
                c.setReadTimeout(timeoutMs);
                c.setUseCaches(false);
                c.setInstanceFollowRedirects(false);
                c.setRequestMethod(method);
                if ("POST".equals(method)) {
                    byte[] b = (body == null ? "" : body).getBytes(StandardCharsets.UTF_8);
                    c.setDoOutput(true);
                    c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded; charset=utf-8");
                    c.setFixedLengthStreamingMode(b.length);
                    try (OutputStream os = c.getOutputStream()) { os.write(b); }
                }
                int code = c.getResponseCode();
                InputStream is = code >= 400 ? c.getErrorStream() : c.getInputStream();
                String text = "";
                if (is != null) {
                    ByteArrayOutputStream bo = new ByteArrayOutputStream();
                    byte[] buf = new byte[4096]; int r;
                    while ((r = is.read(buf)) > 0) bo.write(buf, 0, r);
                    is.close();
                    text = bo.toString("UTF-8");
                }
                if (n != null) espNet = n;
                return new String[]{String.valueOf(code), text};
            } catch (Exception e) {
                last = e;
            } finally {
                if (c != null) c.disconnect();
            }
        }
        throw last != null ? last : new Exception("no network");
    }

    /** Join the ESP32 access point (Android 10+ shows a system "connect" dialog). */
    private void connectToEsp(String ssid, String pass) {
        if (Build.VERSION.SDK_INT < 29) {
            toast("اختار شبكة " + ssid + " من الإعدادات (الباسورد " + pass + ")");
            startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS));
            return;
        }
        String perm = Build.VERSION.SDK_INT >= 33 ? Manifest.permission.NEARBY_WIFI_DEVICES
                                                  : Manifest.permission.ACCESS_FINE_LOCATION;
        if (checkSelfPermission(perm) != PackageManager.PERMISSION_GRANTED) {
            pendingSsid = ssid; pendingPass = pass;
            requestPermissions(new String[]{perm, Manifest.permission.ACCESS_FINE_LOCATION}, 7);
            return;
        }
        WifiNetworkSpecifier.Builder b = new WifiNetworkSpecifier.Builder().setSsid(ssid);
        if (pass != null && pass.length() >= 8) b.setWpa2Passphrase(pass);
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .setNetworkSpecifier(b.build())
                .build();
        if (espCallback != null) { try { cm.unregisterNetworkCallback(espCallback); } catch (Exception ignored) { } }
        espCallback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network n) {
                espNet = n;
                cm.bindProcessToNetwork(n);
                runOnUiThread(() -> { toast("✅ اتوصل بـ " + ssid); web.evaluateJavascript("refresh(true)", null); });
            }
            @Override public void onUnavailable() {
                runOnUiThread(() -> toast("ماقدرش يلاقي " + ssid + " — اتأكد إن الـ ESP32 شغال وإن الـ Location مفتوح"));
            }
        };
        try { cm.requestNetwork(req, espCallback, 30000); }
        catch (Exception e) {
            try { cm.requestNetwork(req, espCallback); } catch (Exception e2) { toast("خطأ: " + e2.getMessage()); }
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        super.onRequestPermissionsResult(code, perms, res);
        if (code != 7) return;
        if (res.length > 0 && res[0] == PackageManager.PERMISSION_GRANTED && pendingSsid != null) connectToEsp(pendingSsid, pendingPass);
        else toast("محتاج الإذن عشان يتوصل بشبكة الشاشة");
    }

    private void toast(String m) { Toast.makeText(this, m, Toast.LENGTH_LONG).show(); }

    @Override
    public void onBackPressed() {
        web.evaluateJavascript(
            "(function(){var s=document.getElementById('sheet');if(s&&s.classList.contains('show')){closeSheet();return 1}" +
            "if(typeof curPage!=='undefined'&&curPage!=='home'){go('home');return 1}return 0})()",
            v -> { if ("0".equals(v)) super.onBackPressed(); });
    }

    @Override
    protected void onDestroy() {
        try { if (wifiCallback != null) cm.unregisterNetworkCallback(wifiCallback); } catch (Exception ignored) { }
        try { if (espCallback != null) cm.unregisterNetworkCallback(espCallback); } catch (Exception ignored) { }
        super.onDestroy();
    }

    /** window.Android.* for app.html */
    class Bridge {
        @JavascriptInterface
        public void request(final String id, final String method, final String path, final String body, final int timeoutMs) {
            new Thread(() -> {
                String js;
                try {
                    String[] r = http(method, path, body, timeoutMs > 0 ? timeoutMs : 8000);
                    js = "window.__nr(" + JSONObject.quote(id) + ",true," + r[0] + "," + JSONObject.quote(r[1]) + ")";
                } catch (Exception e) {
                    String m = e.getClass().getSimpleName() + (e.getMessage() != null ? ": " + e.getMessage() : "");
                    js = "window.__nr(" + JSONObject.quote(id) + ",false,0," + JSONObject.quote(m) + ")";
                }
                final String fjs = js;
                runOnUiThread(() -> web.evaluateJavascript(fjs, null));
            }).start();
        }

        @JavascriptInterface
        public String wifiInfo() {
            try {
                JSONObject o = new JSONObject();
                WifiInfo wi = wm.getConnectionInfo();
                String ssid = wi != null ? wi.getSSID() : null;
                if (ssid != null) ssid = ssid.replace("\"", "");
                if ("<unknown ssid>".equals(ssid)) ssid = null;
                o.put("ssid", ssid == null ? "" : ssid);
                o.put("wifiOn", wm.isWifiEnabled());
                o.put("bound", espNet != null);
                Network n = espNet != null ? espNet : cm.getBoundNetworkForProcess();
                if (n != null) {
                    LinkProperties lp = cm.getLinkProperties(n);
                    if (lp != null && !lp.getLinkAddresses().isEmpty()) o.put("ip", lp.getLinkAddresses().get(0).toString());
                }
                return o.toString();
            } catch (Exception e) { return "{}"; }
        }

        @JavascriptInterface public void connectWifi(String ssid, String pass) {
            // if the ESP32 already answers, don't tear down the working connection
            new Thread(() -> {
                boolean ok;
                try { ok = "200".equals(http("GET", "/api/status", null, 2500)[0]); } catch (Exception e) { ok = false; }
                final boolean reachable = ok;
                runOnUiThread(() -> {
                    if (reachable) { toast("✅ متصل بالـ ESP32 بالفعل"); web.evaluateJavascript("refresh(true)", null); }
                    else connectToEsp(ssid, pass);
                });
            }).start();
        }
        @JavascriptInterface public void openWifiSettings() { runOnUiThread(() -> startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS))); }
        @JavascriptInterface public String version() { return "1.4.0"; }
    }
}
