#include "cache.h"
#include "downloader.h"
#include "fallback_catalog.h"

#include <gtk/gtk.h>
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct App {
  GtkWidget* window = nullptr;
  GtkWidget* themeButton = nullptr;
  GtkWidget* onlineLabel = nullptr;
  GtkWidget* productCombo = nullptr;
  GtkWidget* regionCombo = nullptr;
  GtkWidget* versionCombo = nullptr;
  GtkWidget* localeCombo = nullptr;
  GtkWidget* platformCombo = nullptr;
  GtkWidget* archCombo = nullptr;
  GtkWidget* buildEntry = nullptr;
  GtkWidget* cdnEntry = nullptr;
  GtkWidget* outputEntry = nullptr;
  GtkWidget* cacheEntry = nullptr;
  GtkWidget* clearCacheCheck = nullptr;
  GtkWidget* globalSpin = nullptr;
  GtkWidget* perHostSpin = nullptr;
  GtkWidget* speedCombo = nullptr;
  GtkWidget* startButton = nullptr;
  GtkWidget* pauseButton = nullptr;
  GtkWidget* stopButton = nullptr;
  GtkWidget* refreshButton = nullptr;
  GtkWidget* catalogButton = nullptr;
  GtkWidget* showEncryptedCheck = nullptr;
  GtkWidget* productSearchEntry = nullptr;
  GtkWidget* productSearchStatus = nullptr;
  GtkWidget* progressBar = nullptr;
  GtkWidget* statusLabel = nullptr;
  GtkWidget* detailLabel = nullptr;
  GtkWidget* estimateLabel = nullptr;
  GtkWidget* consoleView = nullptr;
  GtkWidget* consoleScroller = nullptr;
  GtkTextBuffer* consoleBuffer = nullptr;
  GtkWidget* copyButton = nullptr;
  GtkWidget* clearButton = nullptr;
  GtkCssProvider* cssProvider = nullptr;
  std::shared_ptr<Downloader> downloader = std::make_shared<Downloader>();
  std::vector<VersionRecord> versions;
  std::vector<Product> products;
  std::vector<ProductRecord> catalogRecords;
  std::string productBeforeSearch;
  std::string productSearchQuery;
  std::map<std::string, std::string> cdnPaths;
  std::thread versionThread;
  std::thread catalogThread;
  size_t versionRequestId = 0;
  std::thread downloadThread;
  std::shared_ptr<std::atomic<bool>> downloadFinished = std::make_shared<std::atomic<bool>>(false);
  std::atomic<bool> closing{false};
  std::shared_ptr<std::atomic<bool>> lifetime = std::make_shared<std::atomic<bool>>(false);
  std::atomic<bool> downloading{false};
  std::atomic<bool> versionLoading{false};
  std::atomic<bool> catalogLoading{false};
  bool updatingProducts = false;
  uint64_t logCount = 0;
  std::string lastAutoOutput;
  bool dark = true;
};

const char* darkCss = R"CSS(
window, .background { background-color:#1b1e23; color:#f2f4f7; font-family:"DejaVu Sans"; font-size:13px; }
.titlebar { background:#202329; color:#f2f4f7; border-bottom:1px solid #343a43; min-height:40px; padding:4px 8px; }
.titlebar label.caption-title { font-size:16px; font-weight:bold; }
.titlebar label.caption-subtitle { font-size:11px; color:#8a929d; }
button.window-control { min-width:32px; min-height:32px; padding:0; background:transparent; border:1px solid transparent; border-radius:4px; font-size:16px; font-weight:normal; }
button.window-control:hover { background:#343a43; }
button.window-control.close:hover { background:#c42b1c; color:#ffffff; }
.panel { background:#23272d; border:1px solid #3a4048; border-radius:8px; padding:18px; }
entry, combobox, spinbutton { background:#2b3037; color:#f2f4f7; border:1px solid #454c55; border-radius:6px; padding:6px 8px; }
entry:focus, combobox:focus, spinbutton:focus { border-color:#1f7ae0; }
combobox arrow { min-width:14px; min-height:14px; }
menu { background:#2b3037; color:#f2f4f7; border:1px solid #454c55; }
menu menuitem { padding:6px 10px; }
treeview.view { background:#2b3037; color:#f2f4f7; }
treeview.view:selected { background:#35506b; color:#ffffff; }
button { background:#343a43; color:#f2f4f7; border:1px solid #454c55; border-radius:6px; padding:7px 12px; font-weight:600; }
button:hover { background:#3d444e; }
button:disabled { color:#6f7782; background:#2a2f36; }
button.primary { background:#1f7ae0; border-color:#1f7ae0; color:#ffffff; }
button.primary:hover { background:#3389e6; }
label.title { font-size:28px; font-weight:bold; }
label.version { font-size:13px; color:#8a929d; }
label.section { font-size:14px; font-weight:bold; }
label.field { color:#b8bec7; font-size:12px; }
label.muted { color:#8a929d; font-size:12px; }
label.online { background:#2ea44f; color:#ffffff; border-radius:14px; padding:4px 11px; font-weight:bold; }
progressbar { min-height:18px; border-radius:9px; background:#3b424b; }
progressbar progress { min-height:18px; border-radius:9px; background:#1f7ae0; }
textview { background:#0e1013; color:#e5e7eb; font-family:"DejaVu Sans Mono"; font-size:12px; border:1px solid #343a43; border-radius:6px; padding:8px; }
checkbutton { color:#b8bec7; }
expander { color:#f2f4f7; }
)CSS";

const char* lightCss = R"CSS(
window, .background { background-color:#f3f5f7; color:#1f2937; font-family:"DejaVu Sans"; font-size:13px; }
.titlebar { background:#ffffff; color:#1f2937; border-bottom:1px solid #d7dce2; min-height:40px; padding:4px 8px; }
.titlebar label.caption-title { font-size:16px; font-weight:bold; }
.titlebar label.caption-subtitle { font-size:11px; color:#6b7280; }
button.window-control { min-width:32px; min-height:32px; padding:0; background:transparent; border:1px solid transparent; border-radius:4px; font-size:16px; font-weight:normal; }
button.window-control:hover { background:#e9edf1; }
button.window-control.close:hover { background:#c42b1c; color:#ffffff; }
.panel { background:#ffffff; border:1px solid #d7dce2; border-radius:8px; padding:18px; }
entry, combobox, spinbutton { background:#ffffff; color:#1f2937; border:1px solid #c7ced6; border-radius:6px; padding:6px 8px; }
entry:focus, combobox:focus, spinbutton:focus { border-color:#1f7ae0; }
combobox arrow { min-width:14px; min-height:14px; }
menu { background:#ffffff; color:#1f2937; border:1px solid #c7ced6; }
menu menuitem { padding:6px 10px; }
treeview.view { background:#ffffff; color:#1f2937; }
treeview.view:selected { background:#dbeafe; color:#1f2937; }
button { background:#e9edf1; color:#1f2937; border:1px solid #c7ced6; border-radius:6px; padding:7px 12px; font-weight:600; }
button:hover { background:#dfe5ea; }
button:disabled { color:#98a1ab; background:#f1f3f5; }
button.primary { background:#1f7ae0; border-color:#1f7ae0; color:#ffffff; }
button.primary:hover { background:#3389e6; }
label.title { font-size:28px; font-weight:bold; }
label.version { font-size:13px; color:#6b7280; }
label.section { font-size:14px; font-weight:bold; }
label.field { color:#4b5563; font-size:12px; }
label.muted { color:#6b7280; font-size:12px; }
label.online { background:#2ea44f; color:#ffffff; border-radius:14px; padding:4px 11px; font-weight:bold; }
progressbar { min-height:18px; border-radius:9px; background:#d7dce2; }
progressbar progress { min-height:18px; border-radius:9px; background:#1f7ae0; }
textview { background:#111827; color:#e5e7eb; font-family:"DejaVu Sans Mono"; font-size:12px; border:1px solid #d7dce2; border-radius:6px; padding:8px; }
checkbutton { color:#4b5563; }
expander { color:#1f2937; }
)CSS";

GtkWidget* makeBox(GtkOrientation orientation, int spacing) {
  GtkWidget* box = gtk_box_new(orientation, spacing);
  gtk_widget_set_hexpand(box, TRUE);
  return box;
}

GtkWidget* makeLabel(const char* text, const char* cssClass = nullptr) {
  GtkWidget* label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  if (cssClass) gtk_style_context_add_class(gtk_widget_get_style_context(label), cssClass);
  return label;
}

GtkWidget* makeButton(const char* text, GCallback callback, gpointer data,
                      const char* cssClass = nullptr) {
  GtkWidget* button = gtk_button_new_with_label(text);
  g_signal_connect(button, "clicked", callback, data);
  if (cssClass) gtk_style_context_add_class(gtk_widget_get_style_context(button), cssClass);
  return button;
}

GtkWidget* makeCombo(std::vector<std::string> const& values) {
  GtkWidget* combo = gtk_combo_box_text_new();
  for (auto const& value : values) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), value.c_str());
  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
  return combo;
}

void limitComboTextWidth(GtkWidget* combo, int width) {
  gtk_widget_set_size_request(combo, width, -1);
  gtk_combo_box_set_popup_fixed_width(GTK_COMBO_BOX(combo), TRUE);
  for (GList* item = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo)); item; item = item->next) {
    if (GTK_IS_CELL_RENDERER_TEXT(item->data)) {
      gtk_cell_renderer_set_fixed_size(GTK_CELL_RENDERER(item->data), width - 40, -1);
      g_object_set(item->data, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    }
  }
}

GtkWidget* makeSpin(int min, int max, int value) {
  GtkWidget* spin = gtk_spin_button_new_with_range(min, max, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
  gtk_widget_set_size_request(spin, 82, -1);
  return spin;
}

GtkWidget* makePanel() {
  GtkWidget* panel = makeBox(GTK_ORIENTATION_VERTICAL, 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(panel), "panel");
  return panel;
}

void setMargins(GtkWidget* widget, int start, int top, int end, int bottom) {
  gtk_widget_set_margin_start(widget, start);
  gtk_widget_set_margin_top(widget, top);
  gtk_widget_set_margin_end(widget, end);
  gtk_widget_set_margin_bottom(widget, bottom);
}

std::string homePath(std::string const& suffix) {
  if (char const* home = std::getenv("HOME")) return (fs::path(home) / suffix).string();
  return suffix;
}

std::string resolveUserPath(std::string text) {
  if (text == "~") text = homePath("");
  else if (text.rfind("~/", 0) == 0) text = homePath(text.substr(2));
  return fs::absolute(fs::path(text)).lexically_normal().string();
}

std::string entryText(GtkWidget* entry) {
  return gtk_entry_get_text(GTK_ENTRY(entry));
}

int activeIndex(GtkWidget* combo) {
  return gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
}

void appendConsole(App* app, std::string const& text) {
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(app->consoleBuffer, &end);
  gtk_text_buffer_insert(app->consoleBuffer, &end, text.c_str(), -1);
  gtk_text_buffer_insert(app->consoleBuffer, &end, "\n", -1);
  GtkTextMark* mark = gtk_text_buffer_get_insert(app->consoleBuffer);
  gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(app->consoleView), mark, 0.0, FALSE, 0.0, 1.0);
}

struct ProgressUpdate {
  App* app;
  std::shared_ptr<std::atomic<bool>> lifetime;
  ProgressSnapshot snapshot;
};

gboolean onProgress(gpointer data) {
  std::unique_ptr<ProgressUpdate> update(static_cast<ProgressUpdate*>(data));
  ProgressSnapshot const& snapshot = update->snapshot;
  if (*update->lifetime) return G_SOURCE_REMOVE;
  App* app = update->app;
  if (!app || app->closing) return G_SOURCE_REMOVE;

  while (app->logCount < snapshot.log_lines.size()) {
    appendConsole(app, snapshot.log_lines[app->logCount]);
    ++app->logCount;
  }
  if (!snapshot.stage.empty()) gtk_label_set_text(GTK_LABEL(app->statusLabel), snapshot.stage.c_str());
  gtk_label_set_text(GTK_LABEL(app->estimateLabel),
      ("Est. Size: " + formatBytes(snapshot.bytes_total)).c_str());
  double fraction = snapshot.bytes_total
      ? static_cast<double>(snapshot.bytes_done) / static_cast<double>(snapshot.bytes_total) : 0.0;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progressBar), std::clamp(fraction, 0.0, 1.0));
  char percent[32];
  snprintf(percent, sizeof percent, "%.1f%%", fraction * 100.0);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progressBar), percent);

  std::string details = formatBytes(snapshot.bytes_done) + " / " + formatBytes(snapshot.bytes_total);
  if (snapshot.bytes_per_second) details += " • " + formatBytes(snapshot.bytes_per_second) + "/s";
  if (snapshot.eta_seconds != UINT64_MAX) details += " • " + formatDuration(snapshot.eta_seconds) + " remaining";
  if (snapshot.files_total) details += " • " + std::to_string(snapshot.files_done) + "/" +
                                       std::to_string(snapshot.files_total) + " files";
  gtk_label_set_text(GTK_LABEL(app->detailLabel), details.c_str());

  if (snapshot.finished || snapshot.failed) {
    app->downloading = false;
    gtk_widget_set_sensitive(app->startButton, TRUE);
    gtk_widget_set_sensitive(app->pauseButton, FALSE);
    gtk_widget_set_sensitive(app->stopButton, FALSE);
    gtk_button_set_label(GTK_BUTTON(app->pauseButton), "❚❚  Pause");
    if (snapshot.finished) gtk_style_context_add_class(gtk_widget_get_style_context(app->onlineLabel), "online");
  }
  return G_SOURCE_REMOVE;
}

struct VersionResult {
  App* app = nullptr;
  std::shared_ptr<std::atomic<bool>> lifetime;
  size_t requestId = 0;
  bool ok = false;
  std::string status;
  std::string product;
  std::string region;
  std::string cdnPath;
  std::vector<VersionRecord> records;
};

gboolean onVersions(gpointer data) {
  std::unique_ptr<VersionResult> result(static_cast<VersionResult*>(data));
  if (*result->lifetime) return G_SOURCE_REMOVE;
  App* app = result->app;
  if (!app || app->closing || result->requestId != app->versionRequestId) return G_SOURCE_REMOVE;
  int productIndex = activeIndex(app->productCombo);
  if (productIndex < 0 || productIndex >= static_cast<int>(app->products.size())) return G_SOURCE_REMOVE;
  std::string currentProduct = app->products[productIndex].code;
  std::string currentRegion = activeIndex(app->regionCombo) == 0 ? "us" : "eu";
  if (result->product != currentProduct || result->region != currentRegion) return G_SOURCE_REMOVE;
  app->versionLoading = false;
  gtk_widget_set_sensitive(app->refreshButton, TRUE);
  gtk_widget_set_sensitive(app->startButton, TRUE);
  if (!result->cdnPath.empty()) app->cdnPaths[result->product + "\n" + result->region] = result->cdnPath;
  if (result->ok) {
    app->versions = std::move(result->records);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->versionCombo));
    for (auto const& record : app->versions) {
      std::string label = record.version;
      if (!record.build_config.empty()) label += " (" + record.build_config.substr(0, 8) + ")";
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->versionCombo), label.c_str());
    }
    if (!app->versions.empty()) gtk_combo_box_set_active(GTK_COMBO_BOX(app->versionCombo), 0);
    gtk_label_set_text(GTK_LABEL(app->onlineLabel), "CDN: Online");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->onlineLabel), "online");
  } else {
    gtk_label_set_text(GTK_LABEL(app->onlineLabel), "CDN: Offline");
    gtk_style_context_remove_class(gtk_widget_get_style_context(app->onlineLabel), "online");
  }
  gtk_label_set_text(GTK_LABEL(app->statusLabel), result->status.c_str());
  appendConsole(app, result->status);
  return G_SOURCE_REMOVE;
}

struct StatusUpdate {
  App* app;
  std::shared_ptr<std::atomic<bool>> lifetime;
  std::string text;
};

gboolean onStatus(gpointer data) {
  std::unique_ptr<StatusUpdate> update(static_cast<StatusUpdate*>(data));
  if (*update->lifetime) return G_SOURCE_REMOVE;
  if (!update->app->closing) gtk_label_set_text(GTK_LABEL(update->app->statusLabel), update->text.c_str());
  return G_SOURCE_REMOVE;
}

void loadVersions(App* app) {
  if (app->closing) return;
  int productIndex = activeIndex(app->productCombo);
  if (productIndex < 0 || productIndex >= static_cast<int>(app->products.size())) return;
  std::string product = app->products[productIndex].code;
  if (product.empty()) return;
  if (app->versionLoading) {
    if (app->versionThread.joinable()) app->versionThread.detach();
    app->versionLoading = false;
  }
  app->versionLoading = true;
  gtk_widget_set_sensitive(app->refreshButton, FALSE);
  gtk_widget_set_sensitive(app->startButton, FALSE);
  gtk_label_set_text(GTK_LABEL(app->statusLabel), "Loading version history...");
  std::string region = activeIndex(app->regionCombo) == 0 ? "us" : "eu";
  std::string cache = entryText(app->cacheEntry);
  if (app->versionThread.joinable()) app->versionThread.detach();
  auto lifetime = app->lifetime;
  size_t requestId = ++app->versionRequestId;
  app->versionThread = std::thread([app, lifetime, product, region, cache, requestId] {
    auto* result = new VersionResult;
    result->app = app;
    result->lifetime = lifetime;
    result->requestId = requestId;
    result->product = product;
    result->region = region;
    try {
      result->cdnPath = Downloader::fetchCdnPath(product, region, cache);
      result->records = Downloader::fetchVersions(product, region, cache,
          [app, lifetime](std::string const& status) {
            auto* update = new StatusUpdate{app, lifetime, status};
            g_idle_add(onStatus, update);
          });
      result->ok = true;
      result->status = "Loaded " + std::to_string(result->records.size()) + " " + product +
                       " versions (" + region + ")";
    } catch (std::exception const& ex) {
      result->status = std::string("Version lookup failed: ") + ex.what();
    } catch (...) {
      result->status = "Version lookup failed";
    }
    g_idle_add(onVersions, result);
  });
}

std::string comboText(GtkWidget* combo) {
  if (GTK_IS_ENTRY(combo)) return gtk_entry_get_text(GTK_ENTRY(combo));
  char* text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
  std::string result = text ? text : "";
  g_free(text);
  return result;
}

void selectVersion(App* app) {
  int index = activeIndex(app->versionCombo);
  if (index < 0 || index >= static_cast<int>(app->versions.size())) return;
  auto const& record = app->versions[index];
  gtk_entry_set_text(GTK_ENTRY(app->buildEntry), record.build_config.c_str());
  gtk_entry_set_text(GTK_ENTRY(app->cdnEntry), record.cdn_config.c_str());
  if (entryText(app->outputEntry) == app->lastAutoOutput) {
    std::string product = app->products[activeIndex(app->productCombo)].code;
    app->lastAutoOutput = homePath("Games/" + product + " " + record.version);
    gtk_entry_set_text(GTK_ENTRY(app->outputEntry), app->lastAutoOutput.c_str());
  }
}

void onVersionChanged(GtkComboBox*, gpointer data) { selectVersion(static_cast<App*>(data)); }

void onProductChanged(GtkComboBox*, gpointer data) {
  App* app = static_cast<App*>(data);
  if (app->updatingProducts) return;
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->versionCombo));
  app->versions.clear();
  loadVersions(app);
}

void onRefresh(GtkButton*, gpointer data) { loadVersions(static_cast<App*>(data)); }

std::string repopulateProductCombo(App* app, std::string const& selected) {
  app->updatingProducts = true;
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->productCombo));
  if (app->products.empty()) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->productCombo), -1);
    limitComboTextWidth(app->productCombo, 300);
    app->updatingProducts = false;
    return "";
  }
  int selectedIndex = 0;
  for (size_t i = 0; i < app->products.size(); ++i) {
    std::string label = app->products[i].encrypted ? "(encrypted) " : "";
    label += app->products[i].name;
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->productCombo), label.c_str());
    if (app->products[i].code == selected) selectedIndex = static_cast<int>(i);
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(app->productCombo), selectedIndex);
  limitComboTextWidth(app->productCombo, 300);
  app->updatingProducts = false;
  return app->products[selectedIndex].code;
}

std::string currentProductCode(App* app) {
  int index = activeIndex(app->productCombo);
  return index >= 0 && index < static_cast<int>(app->products.size()) ? app->products[index].code : "";
}

bool hasProductSearchQuery(App* app) {
  std::string query = gtk_entry_get_text(GTK_ENTRY(app->productSearchEntry));
  return query.find_first_not_of(" \t\r\n") != std::string::npos;
}

void clearProductVersions(App* app) {
  ++app->versionRequestId;
  if (app->versionLoading) {
    if (app->versionThread.joinable()) app->versionThread.detach();
    app->versionLoading = false;
    gtk_widget_set_sensitive(app->refreshButton, TRUE);
  }
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->versionCombo));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->versionCombo), "Select a product");
  gtk_combo_box_set_active(GTK_COMBO_BOX(app->versionCombo), 0);
  app->versions.clear();
  gtk_widget_set_sensitive(app->startButton, FALSE);
}

std::string rebuildProductList(App* app) {
  std::string selected = currentProductCode(app);
  std::vector<Product> fallback(std::begin(fallbackProducts), std::end(fallbackProducts));
  bool showEncrypted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->showEncryptedCheck));
  app->products = mergeProductCatalog(fallback, app->catalogRecords, showEncrypted);
  if (hasProductSearchQuery(app)) app->products = filterProducts(app->products, gtk_entry_get_text(GTK_ENTRY(app->productSearchEntry)));
  std::string actual = repopulateProductCombo(app, selected);
  if (app->productSearchStatus) {
    std::string status = std::to_string(app->products.size()) + (app->products.size() == 1 ? " product" : " products");
    if (app->products.empty()) status = "No matches";
    gtk_label_set_text(GTK_LABEL(app->productSearchStatus), status.c_str());
  }
  if (hasProductSearchQuery(app) && actual != selected) clearProductVersions(app);
  return actual;
}

struct CatalogResult {
  App* app = nullptr;
  std::shared_ptr<std::atomic<bool>> lifetime;
  bool ok = false;
  std::string status;
  std::vector<ProductRecord> records;
};

gboolean onCatalog(gpointer data) {
  std::unique_ptr<CatalogResult> result(static_cast<CatalogResult*>(data));
  if (*result->lifetime) return G_SOURCE_REMOVE;
  App* app = result->app;
  if (!app || app->closing) return G_SOURCE_REMOVE;
  app->catalogLoading = false;
  gtk_widget_set_sensitive(app->catalogButton, TRUE);
  if (result->ok) {
    std::string selected = currentProductCode(app);
    app->catalogRecords = std::move(result->records);
    std::string actual = rebuildProductList(app);
    if (actual != selected && !hasProductSearchQuery(app)) {
      gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->versionCombo));
      app->versions.clear();
      loadVersions(app);
    }
  }
  gtk_label_set_text(GTK_LABEL(app->statusLabel), result->status.c_str());
  appendConsole(app, result->status);
  return G_SOURCE_REMOVE;
}

void refreshCatalog(App* app) {
  if (app->catalogLoading || app->closing) return;
  app->catalogLoading = true;
  gtk_widget_set_sensitive(app->catalogButton, FALSE);
  std::string cache = entryText(app->cacheEntry);
  if (app->catalogThread.joinable()) app->catalogThread.detach();
  auto lifetime = app->lifetime;
  app->catalogThread = std::thread([app, lifetime, cache] {
    auto* result = new CatalogResult;
    result->app = app;
    result->lifetime = lifetime;
    try {
      result->records = Downloader::fetchProductCatalog(cache, [](std::string const&) {});
      result->ok = true;
      result->status = "BlizzTrack catalog updated: " + std::to_string(result->records.size()) + " products";
    } catch (std::exception const& ex) {
      result->status = std::string("Catalog update failed: ") + ex.what();
    } catch (...) {
      result->status = "Catalog update failed";
    }
    g_idle_add(onCatalog, result);
  });
}

void onCatalogButton(GtkButton*, gpointer data) { refreshCatalog(static_cast<App*>(data)); }

void onShowEncryptedToggled(GtkToggleButton*, gpointer data) {
  App* app = static_cast<App*>(data);
  std::string selected = currentProductCode(app);
  std::string actual = rebuildProductList(app);
  if (actual != selected && !hasProductSearchQuery(app)) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->versionCombo));
    app->versions.clear();
    loadVersions(app);
  }
}

void onProductSearchChanged(GtkEditable*, gpointer data) {
  App* app = static_cast<App*>(data);
  bool wasSearching = app->productSearchQuery.find_first_not_of(" \t\r\n") != std::string::npos;
  std::string selected = currentProductCode(app);
  app->productSearchQuery = gtk_entry_get_text(GTK_ENTRY(app->productSearchEntry));
  bool isSearching = hasProductSearchQuery(app);
  if (!wasSearching && isSearching) app->productBeforeSearch = selected;
  rebuildProductList(app);
  if (!isSearching) {
    std::string restore = app->productBeforeSearch.empty() ? selected : app->productBeforeSearch;
    std::string actual = repopulateProductCombo(app, restore);
    if (actual.empty()) clearProductVersions(app);
    else loadVersions(app);
  }
}

void onTheme(GtkButton*, gpointer data) {
  App* app = static_cast<App*>(data);
  app->dark = !app->dark;
  gtk_css_provider_load_from_data(app->cssProvider, app->dark ? darkCss : lightCss, -1, nullptr);
  gtk_button_set_label(GTK_BUTTON(app->themeButton), app->dark ? "☀  Light Mode" : "☾  Dark Mode");
}

std::string chooseDirectory(App* app, const char* title, std::string const& current) {
  GtkWidget* dialog = gtk_file_chooser_dialog_new(title, GTK_WINDOW(app->window),
      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel", GTK_RESPONSE_CANCEL,
      "Select", GTK_RESPONSE_ACCEPT, nullptr);
  if (!current.empty()) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), current.c_str());
  std::string result;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (path) { result = path; g_free(path); }
  }
  gtk_widget_destroy(dialog);
  return result;
}

void onBrowseOutput(GtkButton*, gpointer data) {
  App* app = static_cast<App*>(data);
  std::string path = chooseDirectory(app, "Select output directory", entryText(app->outputEntry));
  if (!path.empty()) gtk_entry_set_text(GTK_ENTRY(app->outputEntry), path.c_str());
}

void onBrowseCache(GtkButton*, gpointer data) {
  App* app = static_cast<App*>(data);
  std::string path = chooseDirectory(app, "Select cache directory", entryText(app->cacheEntry));
  if (!path.empty()) {
    gtk_entry_set_text(GTK_ENTRY(app->cacheEntry), path.c_str());
    loadVersions(app);
    refreshCatalog(app);
  }
}

void onCopyConsole(GtkButton*, gpointer data) {
  App* app = static_cast<App*>(data);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(app->consoleBuffer, &start, &end);
  char* text = gtk_text_buffer_get_text(app->consoleBuffer, &start, &end, FALSE);
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
  g_free(text);
}

void onClearConsole(GtkButton*, gpointer data) {
  App* app = static_cast<App*>(data);
  gtk_text_buffer_set_text(app->consoleBuffer, "", -1);
  app->logCount = 0;
}

void showError(App* app, const char* message) {
  GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), GTK_MESSAGE_ERROR,
      GTK_BUTTONS_CLOSE, "%s", message);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

bool validateCache(App* app, std::string& message) {
  if (entryText(app->cacheEntry).empty()) { message = "Choose a cache directory."; return false; }
  fs::path cache(resolveUserPath(entryText(app->cacheEntry)));
  fs::path output(resolveUserPath(entryText(app->outputEntry)));
  fs::path home(resolveUserPath(homePath("")));
  fs::path current(fs::current_path());
  if (cache == cache.root_path() || cache == home || cache == current) {
    message = "The cache directory cannot be the filesystem, home, or current directory."; return false;
  }
  if (cache == output || output.string().find(cache.string() + "/") == 0) {
    message = "The output directory cannot be inside the cache directory."; return false;
  }
  return true;
}

void startDownload(App* app) {
  if (app->downloading) return;
  int versionIndex = activeIndex(app->versionCombo);
  if (versionIndex < 0 || versionIndex >= static_cast<int>(app->versions.size())) {
    showError(app, "Select a version first."); return;
  }
  std::string cacheError;
  if (!validateCache(app, cacheError)) { showError(app, cacheError.c_str()); return; }
  if (entryText(app->outputEntry).empty()) { showError(app, "Choose an output directory."); return; }
  fs::path outputRoot(resolveUserPath(entryText(app->outputEntry)));
  if (outputRoot == outputRoot.root_path() || outputRoot == fs::path(resolveUserPath(homePath(""))) ||
      outputRoot == fs::current_path()) {
    showError(app, "Choose a dedicated output directory, not a filesystem, home, or current directory.");
    return;
  }

  DownloadOptions options;
  options.product = app->products[activeIndex(app->productCombo)].code;
  options.region = activeIndex(app->regionCombo) == 0 ? "us" : "eu";
  auto pathIt = app->cdnPaths.find(options.product + "\n" + options.region);
  if (pathIt != app->cdnPaths.end()) options.cdn_path = pathIt->second;
  options.platform = activeIndex(app->platformCombo) == 0 ? "Windows" : "OSX";
  options.arch = activeIndex(app->archCombo) == 0 ? "x86_64" : "x86";
  options.locale = comboText(app->localeCombo);
  options.build_config = entryText(app->buildEntry);
  options.cdn_config = entryText(app->cdnEntry);
  options.output_dir = resolveUserPath(entryText(app->outputEntry));
  options.cache_dir = resolveUserPath(entryText(app->cacheEntry));
  options.global_connections = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->globalSpin));
  options.per_host_connections = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->perHostSpin));
  int speedIndex = activeIndex(app->speedCombo);
  const int speeds[] = {0, 5, 15, 30, 50};
  options.speed_cap = speedIndex >= 0 && speedIndex < 5 ? static_cast<uint64_t>(speeds[speedIndex]) * 1024 * 1024 : 0;

  app->downloading = true;
  app->logCount = 0;
  gtk_text_buffer_set_text(app->consoleBuffer, "", -1);
  gtk_widget_set_sensitive(app->startButton, FALSE);
  gtk_widget_set_sensitive(app->pauseButton, TRUE);
  gtk_widget_set_sensitive(app->stopButton, TRUE);
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progressBar), 0.0);
  auto downloader = app->downloader;
  auto lifetime = app->lifetime;
  auto finished = app->downloadFinished;
  downloader->resetState();
  *finished = false;
  downloader->setCallback([app, lifetime](ProgressSnapshot const& snapshot) {
    auto* copy = new ProgressUpdate{app, lifetime, snapshot};
    g_idle_add(onProgress, copy);
  });
  if (app->downloadThread.joinable()) app->downloadThread.detach();
  app->downloadThread = std::thread([downloader, options, finished] {
    downloader->run(options);
    *finished = true;
  });
}

void onStart(GtkButton*, gpointer data) { startDownload(static_cast<App*>(data)); }

void onPause(GtkButton* button, gpointer data) {
  App* app = static_cast<App*>(data);
  app->downloader->requestPause();
  bool paused = app->downloader->isPaused();
  gtk_button_set_label(button, paused ? "▶  Resume" : "❚❚  Pause");
}

void onStop(GtkButton*, gpointer data) {
  App* app = static_cast<App*>(data);
  app->downloader->requestStop();
  gtk_widget_set_sensitive(app->stopButton, FALSE);
}

struct CloseState {
  App* app;
  std::string cachePath;
};

gboolean finishCloseAfterDownload(gpointer data) {
  auto* state = static_cast<CloseState*>(data);
  App* app = state->app;
  if (!*app->downloadFinished) return G_SOURCE_CONTINUE;
  if (app->downloadThread.joinable()) app->downloadThread.join();
  std::string clearError;
  if (!clearBlizzgetCache(state->cachePath, clearError)) showError(app, clearError.c_str());
  app->closing = true;
  *app->lifetime = true;
  if (app->versionThread.joinable()) app->versionThread.detach();
  if (app->catalogThread.joinable()) app->catalogThread.detach();
  delete state;
  gtk_main_quit();
  return G_SOURCE_REMOVE;
}

gboolean onDelete(GtkWidget*, GdkEvent*, gpointer data) {
  App* app = static_cast<App*>(data);
  if (app->downloading) {
    GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE, "Stop the download and close?");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL,
                           "Stop and close", GTK_RESPONSE_ACCEPT, nullptr);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_ACCEPT) return TRUE;
  }

  bool workerActive = app->downloadThread.joinable() && !*app->downloadFinished;
  if (workerActive) app->downloader->requestStop();
  bool clearCache = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->clearCacheCheck));
  std::string cachePath;
  if (clearCache) {
    std::string cacheError;
    if (!validateCache(app, cacheError)) {
      showError(app, cacheError.c_str());
      clearCache = false;
    } else {
      cachePath = resolveUserPath(entryText(app->cacheEntry));
      GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
          static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), GTK_MESSAGE_QUESTION,
          GTK_BUTTONS_NONE, "Clear BlizzGet cache contents?\n\n%s", cachePath.c_str());
      gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL,
                             "Clear cache", GTK_RESPONSE_ACCEPT, nullptr);
      int response = gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
      clearCache = response == GTK_RESPONSE_ACCEPT;
    }
  }

  app->closing = true;
  *app->lifetime = true;
  if (clearCache && workerActive) {
    gtk_widget_hide(app->window);
    g_idle_add(finishCloseAfterDownload, new CloseState{app, cachePath});
    return TRUE;
  }

  if (app->downloadThread.joinable()) {
    if (*app->downloadFinished) app->downloadThread.join();
    else app->downloadThread.detach();
  }
  if (clearCache) {
    std::string clearError;
    if (!clearBlizzgetCache(cachePath, clearError)) showError(app, clearError.c_str());
  }
  if (app->versionThread.joinable()) app->versionThread.detach();
  if (app->catalogThread.joinable()) app->catalogThread.detach();
  gtk_main_quit();
  return FALSE;
}

void addField(GtkWidget* grid, const char* label, GtkWidget* control, int column) {
  GtkWidget* box = makeBox(GTK_ORIENTATION_VERTICAL, 5);
  gtk_grid_attach(GTK_GRID(grid), box, column, 0, 1, 1);
  gtk_box_pack_start(GTK_BOX(box), makeLabel(label, "field"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), control, FALSE, FALSE, 0);
  gtk_widget_set_hexpand(control, TRUE);
}

GtkWidget* makeWindowControl(const char* label, GCallback callback, App* app, bool close = false) {
  GtkWidget* button = makeButton(label, callback, app, "window-control");
  gtk_widget_set_size_request(button, 32, 32);
  if (close) gtk_style_context_add_class(gtk_widget_get_style_context(button), "close");
  return button;
}

void onMinimize(GtkButton*, gpointer data) {
  gtk_window_iconify(GTK_WINDOW(static_cast<App*>(data)->window));
}

void onMaximize(GtkButton*, gpointer data) {
  GtkWindow* window = GTK_WINDOW(static_cast<App*>(data)->window);
  if (gtk_window_is_maximized(window)) gtk_window_unmaximize(window);
  else gtk_window_maximize(window);
}

void onWindowClose(GtkButton*, gpointer data) {
  gtk_window_close(GTK_WINDOW(static_cast<App*>(data)->window));
}

gboolean onTitlebarPress(GtkWidget*, GdkEventButton* event, gpointer data) {
  if (event->button != 1) return FALSE;
  GtkWindow* window = GTK_WINDOW(static_cast<App*>(data)->window);
  if (event->type == GDK_2BUTTON_PRESS) {
    if (gtk_window_is_maximized(window)) gtk_window_unmaximize(window);
    else gtk_window_maximize(window);
  } else {
    gtk_window_begin_move_drag(window, event->button, event->x_root, event->y_root, event->time);
  }
  return TRUE;
}

GtkWidget* buildUi(App* app) {
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  app->window = window;
  gtk_window_set_title(GTK_WINDOW(window), "BlizzGet");
  gtk_window_set_default_size(GTK_WINDOW(window), 1280, 900);
  gtk_widget_set_size_request(window, 1120, 760);
  g_object_set_data(G_OBJECT(window), "app", app);
  g_signal_connect(window, "delete-event", G_CALLBACK(onDelete), app);

  GtkWidget* titleBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(titleBar), "titlebar");
  gtk_widget_set_size_request(titleBar, -1, 40);
  GtkWidget* dragArea = gtk_event_box_new();
  gtk_widget_set_hexpand(dragArea, TRUE);
  gtk_box_pack_start(GTK_BOX(titleBar), dragArea, TRUE, TRUE, 0);
  GtkWidget* captionBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(captionBox, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(captionBox, GTK_ALIGN_CENTER);
  gtk_container_add(GTK_CONTAINER(dragArea), captionBox);
  gtk_box_pack_start(GTK_BOX(captionBox), makeLabel("BlizzGet", "caption-title"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(captionBox), makeLabel("Official Blizzard CDN Downloader", "caption-subtitle"), FALSE, FALSE, 0);
  g_signal_connect(dragArea, "button-press-event", G_CALLBACK(onTitlebarPress), app);

  GtkWidget* windowControls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign(windowControls, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(windowControls), makeWindowControl("−", G_CALLBACK(onMinimize), app), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(windowControls), makeWindowControl("□", G_CALLBACK(onMaximize), app), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(windowControls), makeWindowControl("×", G_CALLBACK(onWindowClose), app, true), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(titleBar), windowControls, FALSE, FALSE, 0);
  gtk_window_set_titlebar(GTK_WINDOW(window), titleBar);

  GtkWidget* root = makeBox(GTK_ORIENTATION_VERTICAL, 10);
  setMargins(root, 24, 20, 24, 14);
  gtk_container_add(GTK_CONTAINER(window), root);

  GtkWidget* header = makeBox(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);
  GtkWidget* titleBox = makeBox(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(header), titleBox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(titleBox), makeLabel("BlizzGet", "title"), FALSE, FALSE, 0);
  GtkWidget* version = makeLabel("v1.2", "version");
  gtk_widget_set_valign(version, GTK_ALIGN_BASELINE);
  gtk_box_pack_start(GTK_BOX(titleBox), version, FALSE, FALSE, 0);
  app->onlineLabel = makeLabel("CDN: Online", "online");
  gtk_box_pack_end(GTK_BOX(header), app->onlineLabel, FALSE, FALSE, 0);
  app->themeButton = makeButton("☀  Light Mode", G_CALLBACK(onTheme), app);
  gtk_box_pack_end(GTK_BOX(header), app->themeButton, FALSE, FALSE, 0);

  GtkWidget* buildPanel = makePanel();
  gtk_box_pack_start(GTK_BOX(root), buildPanel, FALSE, FALSE, 0);
  GtkWidget* buildHeading = makeBox(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(buildPanel), buildHeading, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(buildHeading), makeLabel("1. BUILD SELECTION", "section"), FALSE, FALSE, 0);
  app->showEncryptedCheck = gtk_check_button_new_with_label("Show encrypted products");
  gtk_box_pack_start(GTK_BOX(buildHeading), app->showEncryptedCheck, FALSE, FALSE, 0);
  g_signal_connect(app->showEncryptedCheck, "toggled", G_CALLBACK(onShowEncryptedToggled), app);
  app->catalogButton = makeButton("⇩  Update Catalog", G_CALLBACK(onCatalogButton), app);
  gtk_box_pack_end(GTK_BOX(buildHeading), app->catalogButton, FALSE, FALSE, 0);
  app->refreshButton = makeButton("↻  Refresh", G_CALLBACK(onRefresh), app);
  gtk_box_pack_end(GTK_BOX(buildHeading), app->refreshButton, FALSE, FALSE, 0);

  GtkWidget* productSearchRow = makeBox(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(buildPanel), productSearchRow, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(productSearchRow), makeLabel("Search", "field"), FALSE, FALSE, 0);
  app->productSearchEntry = gtk_search_entry_new();
  gtk_widget_set_hexpand(app->productSearchEntry, TRUE);
  gtk_widget_set_size_request(app->productSearchEntry, 360, -1);
  gtk_entry_set_placeholder_text(GTK_ENTRY(app->productSearchEntry), "Search by product name or code");
  gtk_box_pack_start(GTK_BOX(productSearchRow), app->productSearchEntry, TRUE, TRUE, 0);
  app->productSearchStatus = makeLabel("", "muted");
  gtk_widget_set_size_request(app->productSearchStatus, 90, -1);
  gtk_box_pack_end(GTK_BOX(productSearchRow), app->productSearchStatus, FALSE, FALSE, 0);
  g_signal_connect(app->productSearchEntry, "changed", G_CALLBACK(onProductSearchChanged), app);

  GtkWidget* fields = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(fields), 16);
  gtk_grid_set_row_spacing(GTK_GRID(fields), 6);
  gtk_box_pack_start(GTK_BOX(buildPanel), fields, FALSE, FALSE, 0);
  app->products.assign(std::begin(fallbackProducts), std::end(fallbackProducts));
  gtk_label_set_text(GTK_LABEL(app->productSearchStatus), (std::to_string(app->products.size()) + " products").c_str());
  std::vector<std::string> productNames;
  for (auto const& product : app->products) productNames.emplace_back(product.name);
  app->productCombo = makeCombo(productNames);
  app->regionCombo = makeCombo({"US", "EU"});
  app->versionCombo = makeCombo({"Loading..."});
  app->localeCombo = gtk_combo_box_text_new_with_entry();
  for (char const* locale : {"enUS", "enGB", "deDE", "frFR", "esES", "esMX", "itIT", "koKR", "plPL", "ptBR", "ruRU", "zhCN", "zhTW"})
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->localeCombo), locale);
  gtk_combo_box_set_active(GTK_COMBO_BOX(app->localeCombo), 0);
  app->platformCombo = makeCombo({"Windows", "OSX"});
  app->archCombo = makeCombo({"x86_64", "x86"});
  addField(fields, "Product", app->productCombo, 0);
  addField(fields, "Region", app->regionCombo, 1);
  addField(fields, "Version", app->versionCombo, 2);
  addField(fields, "Locale", app->localeCombo, 3);
  addField(fields, "Platform", app->platformCombo, 4);
  addField(fields, "Arch", app->archCombo, 5);
  limitComboTextWidth(app->productCombo, 300);
  gtk_widget_set_size_request(app->regionCombo, 90, -1);
  gtk_widget_set_size_request(app->versionCombo, 230, -1);
  gtk_widget_set_size_request(app->localeCombo, 105, -1);
  gtk_widget_set_size_request(app->platformCombo, 170, -1);
  gtk_widget_set_size_request(app->archCombo, 160, -1);
  gtk_widget_set_hexpand(app->productCombo, TRUE);
  gtk_widget_set_hexpand(app->versionCombo, TRUE);
  gtk_widget_set_hexpand(app->archCombo, TRUE);
  g_signal_connect(app->productCombo, "changed", G_CALLBACK(onProductChanged), app);
  g_signal_connect(app->regionCombo, "changed", G_CALLBACK(onProductChanged), app);
  g_signal_connect(app->versionCombo, "changed", G_CALLBACK(onVersionChanged), app);

  GtkWidget* advanced = gtk_expander_new("▶  Advanced: View Build/CDN Hashes");
  gtk_box_pack_start(GTK_BOX(buildPanel), advanced, FALSE, FALSE, 0);
  GtkWidget* advancedGrid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(advancedGrid), 12);
  gtk_container_set_border_width(GTK_CONTAINER(advancedGrid), 8);
  gtk_container_add(GTK_CONTAINER(advanced), advancedGrid);
  app->buildEntry = gtk_entry_new();
  app->cdnEntry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(app->buildEntry), "5194054cfa6480884c9a451b6f1107a3");
  gtk_entry_set_text(GTK_ENTRY(app->cdnEntry), "6547fc64ca796c7c1b25864145814ace");
  gtk_grid_attach(GTK_GRID(advancedGrid), makeLabel("Build config", "field"), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(advancedGrid), app->buildEntry, 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(advancedGrid), makeLabel("CDN config", "field"), 2, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(advancedGrid), app->cdnEntry, 3, 0, 1, 1);
  gtk_widget_set_hexpand(app->buildEntry, TRUE);
  gtk_widget_set_hexpand(app->cdnEntry, TRUE);

  GtkWidget* destinationPanel = makePanel();
  gtk_box_pack_start(GTK_BOX(root), destinationPanel, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(destinationPanel), makeLabel("2. DESTINATION & NETWORK", "section"), FALSE, FALSE, 0);
  GtkWidget* form = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(form), 10);
  gtk_grid_set_column_spacing(GTK_GRID(form), 12);
  gtk_box_pack_start(GTK_BOX(destinationPanel), form, FALSE, FALSE, 0);
  app->outputEntry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(app->outputEntry), homePath("Games/d3 2.7.4.84161").c_str());
  app->lastAutoOutput = entryText(app->outputEntry);
  app->cacheEntry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(app->cacheEntry), defaultCacheDirectory().c_str());
  gtk_grid_attach(GTK_GRID(form), makeLabel("Output Directory:", "field"), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(form), app->outputEntry, 1, 0, 1, 1);
  GtkWidget* outputBrowse = makeButton("Browse...", G_CALLBACK(onBrowseOutput), app);
  gtk_grid_attach(GTK_GRID(form), outputBrowse, 2, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(form), makeLabel("Cache Directory:", "field"), 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(form), app->cacheEntry, 1, 1, 1, 1);
  GtkWidget* cacheBrowse = makeButton("Browse...", G_CALLBACK(onBrowseCache), app);
  gtk_grid_attach(GTK_GRID(form), cacheBrowse, 2, 1, 1, 1);
  app->clearCacheCheck = gtk_check_button_new_with_label("Clear cache on close");
  gtk_grid_attach(GTK_GRID(form), app->clearCacheCheck, 3, 1, 1, 1);
  gtk_widget_set_hexpand(app->outputEntry, TRUE);
  gtk_widget_set_hexpand(app->cacheEntry, TRUE);

  GtkWidget* network = makeBox(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(destinationPanel), network, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(network), makeLabel("Connections: Global", "field"), FALSE, FALSE, 0);
  app->globalSpin = makeSpin(1, 512, 48);
  gtk_box_pack_start(GTK_BOX(network), app->globalSpin, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(network), makeLabel("Per Host", "field"), FALSE, FALSE, 0);
  app->perHostSpin = makeSpin(1, 64, 12);
  gtk_box_pack_start(GTK_BOX(network), app->perHostSpin, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(network), makeLabel("Speed Cap", "field"), FALSE, FALSE, 0);
  app->speedCombo = makeCombo({"Unlimited", "5 MB/s", "15 MB/s", "30 MB/s", "50 MB/s"});
  gtk_box_pack_start(GTK_BOX(network), app->speedCombo, FALSE, FALSE, 0);

  GtkWidget* downloadPanel = makePanel();
  gtk_box_pack_start(GTK_BOX(root), downloadPanel, FALSE, FALSE, 0);
  GtkWidget* actions = makeBox(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_pack_start(GTK_BOX(downloadPanel), actions, FALSE, FALSE, 0);
  app->startButton = makeButton("↓  Start Download", G_CALLBACK(onStart), app, "primary");
  app->pauseButton = makeButton("❚❚  Pause", G_CALLBACK(onPause), app);
  app->stopButton = makeButton("■  Stop", G_CALLBACK(onStop), app);
  gtk_widget_set_sensitive(app->pauseButton, FALSE);
  gtk_widget_set_sensitive(app->stopButton, FALSE);
  gtk_box_pack_start(GTK_BOX(actions), app->startButton, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(actions), app->pauseButton, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(actions), app->stopButton, FALSE, FALSE, 0);

  GtkWidget* statusRow = makeBox(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(downloadPanel), statusRow, FALSE, FALSE, 0);
  app->statusLabel = makeLabel("Ready", "muted");
  gtk_box_pack_start(GTK_BOX(statusRow), app->statusLabel, FALSE, FALSE, 0);
  GtkWidget* estimate = makeLabel("Est. Size: --", "muted");
  app->estimateLabel = estimate;
  gtk_box_pack_end(GTK_BOX(statusRow), estimate, FALSE, FALSE, 0);
  app->progressBar = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progressBar), TRUE);
  gtk_box_pack_start(GTK_BOX(downloadPanel), app->progressBar, FALSE, FALSE, 0);
  app->detailLabel = makeLabel("0 B / 0 B", "muted");
  gtk_box_pack_start(GTK_BOX(downloadPanel), app->detailLabel, FALSE, FALSE, 0);

  GtkWidget* consolePanel = makePanel();
  gtk_widget_set_vexpand(consolePanel, TRUE);
  gtk_box_pack_start(GTK_BOX(root), consolePanel, TRUE, TRUE, 0);
  GtkWidget* consoleHeader = makeBox(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(consolePanel), consoleHeader, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(consoleHeader), makeLabel("▼  Console Output", "section"), FALSE, FALSE, 0);
  app->copyButton = makeButton("Copy", G_CALLBACK(onCopyConsole), app);
  app->clearButton = makeButton("Clear", G_CALLBACK(onClearConsole), app);
  gtk_box_pack_end(GTK_BOX(consoleHeader), app->copyButton, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(consoleHeader), app->clearButton, FALSE, FALSE, 0);
  app->consoleView = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(app->consoleView), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->consoleView), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->consoleView), TRUE);
  app->consoleBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->consoleView));
  app->consoleScroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->consoleScroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(app->consoleScroller), app->consoleView);
  gtk_widget_set_vexpand(app->consoleScroller, TRUE);
  gtk_box_pack_start(GTK_BOX(consolePanel), app->consoleScroller, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(root), makeLabel(
      "Downloads official Blizzard CDN files. Ownership of the game is required.", "muted"), FALSE, FALSE, 0);
  return window;
}

}

int main(int argc, char** argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  gtk_init(&argc, &argv);
  App app;
  app.cssProvider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(app.cssProvider, darkCss, -1, nullptr);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
      GTK_STYLE_PROVIDER(app.cssProvider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  GtkWidget* window = buildUi(&app);
  gtk_widget_show_all(window);
  loadVersions(&app);
  refreshCatalog(&app);
  gtk_main();
  return 0;
}
