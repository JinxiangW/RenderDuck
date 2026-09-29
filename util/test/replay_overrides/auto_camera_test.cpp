// Tests the native panel + embedded Python with owned copies of real captures.
#define ScheduleReplayOverrideTest ScheduleMouseRegression
#include "mouse_test.cpp"
#undef ScheduleReplayOverrideTest
#include <QDir>
#include <QDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>

static void WaitIO(ITextureViewer *viewer)
{
  for(int i = 0; i < 600; i++)
  {
    Pump(25);
    if(!State(viewer)[QStringLiteral("saving_configuration")].toBool())
      return;
  }
  throw std::runtime_error("configuration I/O timed out");
}
static QByteArray Read(const QString &path)
{
  QFile file(path);
  Check(file.open(QIODevice::ReadOnly), "could not read test file");
  return file.readAll();
}
static void Write(const QString &path, const QByteArray &bytes)
{
  QFile file(path);
  Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "could not write owned test file");
  Check(file.write(bytes) == bytes.size(), "short test write");
}
static void Reload(ICaptureContext *ctx, const QString &path)
{
  ctx->CloseCapture();
  Pump(30);
  ctx->LoadCapture(path, ReplayOptions(), path, false, true);
  Check(ctx->IsCaptureLoaded(), "capture reload failed");
  WaitIO(ctx->GetTextureViewer());
}
static void Scan(ITextureViewer *viewer, QPushButton *button)
{
  button->click();
  for(int i = 0; i < 2400; i++)
  {
    Pump(25);
    if(!State(viewer)[QStringLiteral("scanning")].toBool())
      return;
  }
  Write(QString::fromUtf8(qgetenv("REPLAY_OVERRIDE_TEST_OUTPUT")) + QStringLiteral("/scan-timeout.json"), QJsonDocument(State(viewer)).toJson());
  throw std::runtime_error("native discovery timed out");
}
static void AutoRun(ICaptureContext *ctx, const QString &out)
{
  QString capture = QString(ctx->GetCaptureFilename());
  Check(capture.startsWith(out) && capture.endsWith(QStringLiteral(".rdc")),
        "auto test must use an owned capture copy");
  ITextureViewer *viewer = ctx->GetTextureViewer();
  ctx->ShowTextureViewer();
  WaitIO(viewer);
  QWidget *widget = viewer->Widget();
  auto automatic = widget->findChild<QPushButton *>(QStringLiteral("replayCameraAutoConfigure"));
  auto cancel = widget->findChild<QPushButton *>(QStringLiteral("replayCameraCancelScan"));
  auto partial = widget->findChild<QPushButton *>(QStringLiteral("replayCameraApplyPartial"));
  Check(automatic && cancel && partial, "automatic controls missing from camera panel");
  uint32_t seed = qgetenv("REPLAY_CAMERA_SEED").toUInt();
  ctx->SetEventID({}, seed, seed, true);
  bool opened = false;
  for(QPushButton *button : widget->findChildren<QPushButton *>())
    if(button->text() == QStringLiteral("Free camera..."))
    {
      Check(button->isEnabled(), "Free camera toolbar button is disabled for D3D11");
      button->click();
      opened = true;
    }
  Check(opened && State(viewer)[QStringLiteral("panel_visible")].toBool(),
        "could not open the native free-camera window");
  QJsonObject before = State(viewer);
  automatic->click();
  Pump(50);    // Cancel a live batch, not only a request that has not started.
  cancel->click();
  for(int i = 0; i < 400 && State(viewer)[QStringLiteral("scanning")].toBool(); i++)
    Pump(25);
  Check(!State(viewer)[QStringLiteral("scanning")].toBool(), "scan cancellation timed out");
  Check(State(viewer)[QStringLiteral("fields")] == before[QStringLiteral("fields")],
        "cancellation changed fields");
  Check(ctx->CurEvent() == seed, "cancellation changed selected event");
  Scan(viewer, automatic);
  Write(out + QStringLiteral("/after-scan.json"), QJsonDocument(State(viewer)).toJson());
  Write(out + QStringLiteral("/scan-report.txt"),
        State(viewer)[QStringLiteral("scan_report")].toString().toUtf8());
  const ActionDescription *last = ctx->GetLastAction();
  Check(bool(last->flags & ActionFlags::Present), "fixture does not end with Present");
  ResourceId finalTexture = last->copyDestination;
  auto pixels = [&](const QString &name) {
    bytebuf bytes;
    ctx->Replay().BlockInvoke([&](IReplayController *r) {
      r->SetFrameEvent(last->eventId, true);
      bytes = r->GetTextureData(finalTexture, Subresource());
      TextureSave save;
      save.resourceId = finalTexture;
      save.destType = FileType::PNG;
      Check(r->SaveTexture(save, out + QStringLiteral("/") + name + QStringLiteral(".png")).OK(),
            "could not save evidence image");
      r->SetFrameEvent(ctx->CurEvent(), true);
    });
    return bytes;
  };
  bytebuf baseline = pixels(QStringLiteral("baseline"));
  int heartbeat = 0;
  QTimer responsive;
  responsive.setInterval(1);
  QObject::connect(&responsive, &QTimer::timeout, [&heartbeat]() { heartbeat++; });
  responsive.start();
  if(partial->isEnabled())
  {
    Check(!State(viewer)[QStringLiteral("enabled")].toBool(), "partial scan auto-enabled");
    Check(State(viewer)[QStringLiteral("fields")] == before[QStringLiteral("fields")],
          "unaccepted partial scan changed fields");
    partial->click();
    Check(State(viewer)[QStringLiteral("applying")].toBool(),
          "partial Apply did not start progress immediately");
    auto progress = widget->findChild<QProgressBar *>(QStringLiteral("replayCameraApplyProgress"));
    Check(progress && !progress->isHidden() && progress->minimum() == 0 && progress->maximum() == 0,
          "Apply replay needs indeterminate progress");
    Check(!partial->isEnabled(), "duplicate partial Apply is not disabled");
    partial->click();
  }
  Settled(viewer);
  responsive.stop();
  Check(heartbeat > 1, "UI event loop stopped while applying camera");
  QJsonObject active = State(viewer);
  Check(!active[QStringLiteral("panel_visible")].toBool(),
        "successful partial Apply did not close panel");
  Check(active[QStringLiteral("apply_phase")].toString() == QStringLiteral("complete") &&
            active[QStringLiteral("last_update_ms")].toDouble() > 0,
        "Apply phase/timing not reported");
  Check(active[QStringLiteral("enabled")].toBool(), "scan did not configure the camera");
  Check(active[QStringLiteral("fields")].toArray().size() > 20, "too few confirmed camera fields");
  bytebuf configured = pixels(QStringLiteral("configured"));
  Check(configured.size() == baseline.size() && !baseline.empty(), "image readback failed");
  double difference = 0;
  for(size_t i = 0; i < baseline.size(); i++)
    difference += qAbs(int(configured[i]) - int(baseline[i]));
  difference /= baseline.size();
  Write(out + QStringLiteral("/image-metrics.txt"), QByteArray::number(difference, 'g', 9));
  Check(difference < 0.25, "enabling original camera changed the baseline image materially");
  WaitIO(viewer);
  QString sidecar = capture + QStringLiteral(".freecamera.json");
  QByteArray saved = Read(sidecar);
  auto document = QJsonDocument::fromJson(saved).object();
  auto config = document[QStringLiteral("configuration")].toObject();
  Check(!config.contains(QStringLiteral("enabled")) &&
            !config.contains(QStringLiteral("current_view")),
        "sidecar persisted interactive state");
  Check(config[QStringLiteral("fields")] == active[QStringLiteral("fields")],
        "independent projection/fields did not round trip");
  QWidget *render = widget->findChild<QWidget *>(QStringLiteral("render"));
  Drag(viewer, Surface(render));
  Check(pixels(QStringLiteral("rotated")) != configured, "rotation did not change the final image");
  Pump(100);
  Check(Read(sidecar) == saved, "movement rewrote the baseline configuration");
  viewer->ResetReplayCamera();
  Settled(viewer);
  Check(pixels(QStringLiteral("restored")) == baseline,
        "reset did not reproduce the exact original image");
  Reload(ctx, capture);
  auto restored = State(viewer);
  Check(!restored[QStringLiteral("enabled")].toBool(), "reopen enabled camera without user action");
  Check(restored[QStringLiteral("fields")] == active[QStringLiteral("fields")],
        "reopen failed to restore camera fields");
  Check(restored[QStringLiteral("view")] == active[QStringLiteral("view")],
        "reopen restored moved pose instead of baseline");
  config[QStringLiteral("show")] = false;
  config.remove(QStringLiteral("speed"));
  config.remove(QStringLiteral("forward"));
  Check(viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(config).toJson())).empty(),
        "optional defaults are invalid");
  Settled(viewer);
  WaitIO(viewer);
  // A bad seed must preserve the existing enabled configuration and saved file.
  saved = Read(sidecar);
  const QJsonObject good = State(viewer);
  ctx->SetEventID({}, ctx->GetLastAction()->eventId, ctx->GetLastAction()->eventId, true);
  Scan(viewer, automatic);
  Check(State(viewer)[QStringLiteral("fields")] == good[QStringLiteral("fields")] &&
            State(viewer)[QStringLiteral("enabled")].toBool(),
        "bad seed changed active camera");
  Check(Read(sidecar) == saved, "bad seed overwrote sidecar");
  // Manual Apply uses the same progress path; validation failure must leave the window open.
  for(QPushButton *button : widget->findChildren<QPushButton *>())
    if(button->text() == QStringLiteral("Free camera..."))
      button->click();
  auto apply = widget->findChild<QPushButton *>(QStringLiteral("replayCameraApplyEnable"));
  auto view = widget->findChild<QPlainTextEdit *>(QStringLiteral("replayBaseView"));
  QString validView = view->toPlainText();
  view->setPlainText(QStringLiteral("invalid matrix"));
  apply->click();
  Pump(50);
  Check(State(viewer)[QStringLiteral("apply_phase")].toString() == QStringLiteral("failed") &&
            State(viewer)[QStringLiteral("panel_visible")].toBool(),
        "failed Apply closed its error window");
  view->setPlainText(validView);
  apply->click();
  Check(!apply->isEnabled(), "duplicate manual Apply is not disabled");
  Settled(viewer);
  Check(!State(viewer)[QStringLiteral("panel_visible")].toBool(),
        "successful manual Apply did not close panel");
  WaitIO(viewer);
  // Hold the replay queue briefly so capture close occurs during a real queued Apply.
  ctx->Replay().AsyncInvoke([](IReplayController *) { QThread::msleep(80); });
  apply->click();
  Pump(10);
  Check(State(viewer)[QStringLiteral("applying")].toBool(), "fixture missed in-flight Apply");
  Reload(ctx, capture);
  Check(!State(viewer)[QStringLiteral("applying")].toBool() &&
            !State(viewer)[QStringLiteral("enabled")].toBool(),
        "late Apply completion survived capture reload");
  ctx->SetEventID({}, seed, seed, true);
  automatic->click();
  Pump(50);
  Reload(ctx, capture);    // Close while a scan batch is in flight.
  Check(!State(viewer)[QStringLiteral("scanning")].toBool() &&
            !State(viewer)[QStringLiteral("enabled")].toBool(),
        "old scan survived capture reload");
  Check(State(viewer)[QStringLiteral("fields")] == good[QStringLiteral("fields")],
        "stale scan replaced restored fields");
  // Broken and mismatched files must never be applied.
  ctx->CloseCapture();
  Write(sidecar, "{broken");
  Reload(ctx, capture);
  Check(State(viewer)[QStringLiteral("fields")].toArray().isEmpty(), "broken JSON was applied");
  ctx->CloseCapture();
  document = QJsonDocument::fromJson(saved).object();
  document[QStringLiteral("version")] = 999;
  Write(sidecar, QJsonDocument(document).toJson());
  Reload(ctx, capture);
  Check(State(viewer)[QStringLiteral("fields")].toArray().isEmpty(),
        "unsupported version was applied");
  ctx->CloseCapture();
  document = QJsonDocument::fromJson(saved).object();
  QJsonObject invalidConfig = document[QStringLiteral("configuration")].toObject();
  QJsonArray invalidFields = invalidConfig[QStringLiteral("fields")].toArray();
  QJsonObject invalidField = invalidFields[0].toObject();
  invalidField[QStringLiteral("shader")] = invalidConfig[QStringLiteral("output_rid")];
  invalidFields[0] = invalidField;
  invalidConfig[QStringLiteral("fields")] = invalidFields;
  document[QStringLiteral("configuration")] = invalidConfig;
  Write(sidecar, QJsonDocument(document).toJson());
  Reload(ctx, capture);
  Check(State(viewer)[QStringLiteral("fields")].toArray().isEmpty(),
        "non-shader resource accepted as a shader on restore");
  ctx->CloseCapture();
  document = QJsonDocument::fromJson(saved).object();
  QJsonObject fingerprint = document[QStringLiteral("capture")].toObject();
  fingerprint[QStringLiteral("sha256")] = QStringLiteral("wrong capture");
  document[QStringLiteral("capture")] = fingerprint;
  Write(sidecar, QJsonDocument(document).toJson());
  Reload(ctx, capture);
  Check(State(viewer)[QStringLiteral("fields")].toArray().isEmpty(),
        "different fingerprint was applied");
  ctx->CloseCapture();
  Write(sidecar, saved);
  // Move + rename both files; the fingerprint must be independent of their paths.
  const QString moved = out + QStringLiteral("/renamed.rdc");
  Check(QFile::rename(capture, moved) &&
            QFile::rename(sidecar, moved + QStringLiteral(".freecamera.json")),
        "could not rename owned capture/configuration");
  Reload(ctx, moved);
  Check(State(viewer)[QStringLiteral("fields")] == active[QStringLiteral("fields")],
        "renaming the capture and sidecar broke restore");
  ctx->CloseCapture();
  Check(QFile::rename(moved, capture) &&
            QFile::rename(moved + QStringLiteral(".freecamera.json"), sidecar),
        "could not restore test filenames");
  // A directory occupying the sidecar path deterministically prevents atomic replacement.
  Check(QFile::remove(sidecar) && QDir().mkdir(sidecar), "could not set up unwritable destination");
  Reload(ctx, capture);
  Check(viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(config).toJson())).empty(),
        "save failure prevented configuring camera");
  Settled(viewer);
  WaitIO(viewer);
  Check(State(viewer)[QStringLiteral("enabled")].toBool(), "save failure disabled camera");
  Check(State(viewer)[QStringLiteral("configuration_status")].toString().contains(
            QStringLiteral("not saved")),
        "save failure was not reported");
  auto saveStatus = widget->findChild<QLabel *>(QStringLiteral("replayCameraSaveStatus"));
  Check(saveStatus && !saveStatus->text().isEmpty(), "save failure has no toolbar indication");
  ctx->CloseCapture();
  Check(QDir().rmdir(sidecar), "could not remove owned test directory");
  Write(sidecar, saved);
}

static void RecipeRun(ICaptureContext *ctx, const QString &out)
{
  const QString capture = ctx->GetCaptureFilename();
  Check(capture.startsWith(out), "recipe test requires an owned capture");
  auto viewer = ctx->GetTextureViewer();
  ctx->ShowTextureViewer();
  WaitIO(viewer);
  QJsonObject recipe = QJsonDocument::fromJson(Read(QString::fromUtf8(qgetenv("REPLAY_CAMERA_RECIPE")))).object();
  QJsonObject request{{QStringLiteral("recipe"), recipe}, {QStringLiteral("run_recipe"), false},
                      {QStringLiteral("show"), true}};
  auto configure = [&](const QJsonObject &value) {
    rdcstr error = viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(value).toJson()));
    if(!error.empty()) throw std::runtime_error(error.c_str());
  };
  const auto before = State(viewer);
  configure(request);
  Check(State(viewer)[QStringLiteral("recipe")] == recipe &&
            State(viewer)[QStringLiteral("fields")] == before[QStringLiteral("fields")],
        "storing recipe changed active fields");
  auto widget = viewer->Widget();
  auto editRecipe = widget->findChild<QPushButton *>(QStringLiteral("replayCameraRecipe"));
  Check(editRecipe != nullptr, "recipe editor button missing");
  bool edited = false;
  QTimer editorTimer;
  editorTimer.setInterval(25);
  QObject::connect(&editorTimer, &QTimer::timeout, [&]() {
    auto modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if(!modal) return;
    auto editor = modal->findChild<QPlainTextEdit *>(QStringLiteral("replayCameraRecipeJSON"));
    if(!editor) return;
    editor->setPlainText(QString::fromUtf8(QJsonDocument(recipe).toJson()));
    for(auto button : modal->findChildren<QPushButton *>())
      if(button->text() == QStringLiteral("Use recipe")) { edited = true; button->click(); break; }
  });
  editorTimer.start();
  editRecipe->click();
  editorTimer.stop();
  Check(edited && State(viewer)[QStringLiteral("recipe")] == recipe, "recipe editor did not retain JSON");
  Check(widget->window()->grab().save(out + QStringLiteral("/recipe-window.png")),
        "recipe screenshot failed");
  auto automatic = widget->findChild<QPushButton *>(QStringLiteral("replayCameraAutoConfigure"));
  auto cancel = widget->findChild<QPushButton *>(QStringLiteral("replayCameraCancelScan"));
  auto partial = widget->findChild<QPushButton *>(QStringLiteral("replayCameraApplyPartial"));
  Check(automatic && cancel && partial, "recipe controls missing");
  Check(automatic->text() == QStringLiteral("Auto configure") &&
            partial->text() == QStringLiteral("Apply confirmed configuration"), "non-neutral recipe labels");
  const auto originalEvent = ctx->CurEvent();
  automatic->click();
  Pump(50);
  cancel->click();
  for(int i = 0; i < 2400 && State(viewer)[QStringLiteral("scanning")].toBool(); i++) Pump(25);
  Check(!State(viewer)[QStringLiteral("scanning")].toBool() && ctx->CurEvent() == originalEvent &&
            State(viewer)[QStringLiteral("fields")] == before[QStringLiteral("fields")],
        "cancelled recipe changed fields/event");
  Scan(viewer, automatic);
  const auto scanned = State(viewer);
  Write(out + QStringLiteral("/after-scan.json"), QJsonDocument(scanned).toJson());
  Check(scanned[QStringLiteral("recipe_result_ready")].toBool() &&
            scanned[QStringLiteral("scan_partial")].toBool() && partial->isEnabled(),
        "OW scan did not expose a partial result");
  Check(scanned[QStringLiteral("fields")] == before[QStringLiteral("fields")],
        "unaccepted recipe result changed fields");
  const ActionDescription *last = ctx->GetLastAction();
  Check(bool(last->flags & ActionFlags::Present), "capture has no final Present");
  const ResourceId output = last->copyDestination;
  const uint32_t lastEvent = last->eventId;
  auto pixels = [&](const QString &name) {
    bytebuf bytes;
    ctx->Replay().BlockInvoke([&](IReplayController *r) {
      r->SetFrameEvent(lastEvent, true);
      bytes = r->GetTextureData(output, Subresource());
      TextureSave save;
      save.resourceId = output;
      save.destType = FileType::PNG;
      Check(r->SaveTexture(save, out + QStringLiteral("/") + name + QStringLiteral(".png")).OK(),
            "could not save recipe image");
      r->SetFrameEvent(ctx->CurEvent(), true);
    });
    return bytes;
  };
  auto baseline = pixels(QStringLiteral("baseline"));
  int heartbeat = 0;
  QTimer pulse;
  pulse.setInterval(5);
  QObject::connect(&pulse, &QTimer::timeout, [&]() { heartbeat++; });
  pulse.start();
  partial->click();
  Check(State(viewer)[QStringLiteral("applying")].toBool(), "recipe Apply has no progress");
  Settled(viewer);
  pulse.stop();
  auto active = State(viewer);
  Write(out + QStringLiteral("/active.json"), QJsonDocument(active).toJson());
  const int passCount = recipe[QStringLiteral("passes")].toArray().size();
  Check(active[QStringLiteral("active_pass_count")].toInt() == passCount,
        "recipe passes were not installed with the camera");
  Check(active[QStringLiteral("enabled")].toBool() && !active[QStringLiteral("panel_visible")].toBool() &&
            heartbeat > 1 && ctx->CurEvent() == lastEvent,
        "recipe Apply did not finish/close/refresh asynchronously");
  if(passCount)
  {
    QJsonObject invalid = active;
    auto view = invalid[QStringLiteral("view")].toArray();
    view[3] = view[3].toDouble() + 1;
    invalid[QStringLiteral("view")] = view;
    auto rules = invalid[QStringLiteral("passes")].toArray();
    auto rule = rules[0].toObject();
    auto colours = rule[QStringLiteral("colours")].toArray();
    auto colour = colours[0].toObject();
    colour[QStringLiteral("destination_format")] = 45; // Depth view on the colour destination.
    colours[0] = colour; rule[QStringLiteral("colours")] = colours;
    rules[0] = rule; invalid[QStringLiteral("passes")] = rules;
    configure(invalid);
    for(int i = 0; i < 1000 && State(viewer)[QStringLiteral("pending_update")].toBool(); i++) Pump(10);
    auto failure = State(viewer);
    Check(!failure[QStringLiteral("last_error")].toString().isEmpty() &&
              failure[QStringLiteral("enabled")].toBool() && failure[QStringLiteral("panel_visible")].toBool() &&
              failure[QStringLiteral("active_pass_count")].toInt() == passCount &&
              failure[QStringLiteral("view")] == active[QStringLiteral("view")] &&
              failure[QStringLiteral("passes")] == active[QStringLiteral("passes")],
          "failed combined Apply did not restore the active/visible configuration");
  }
  // Scan batches must restore both active rule sets, including cancellation with a bypass installed.
  automatic->click();
  Pump(50);
  cancel->click();
  for(int i = 0; i < 2400 && State(viewer)[QStringLiteral("scanning")].toBool(); i++) Pump(25);
  Check(!State(viewer)[QStringLiteral("scanning")].toBool() &&
            State(viewer)[QStringLiteral("active_pass_count")].toInt() == passCount &&
            State(viewer)[QStringLiteral("enabled")].toBool(), "scan cancellation did not restore both override sets");
  bool modelFound = false;
  for(auto entry : active[QStringLiteral("fields")].toArray())
    if(entry.toObject()[QStringLiteral("semantic")].toString() == QStringLiteral("ModelView"))
      modelFound = entry.toObject()[QStringLiteral("model")].toArray().size() == 16;
  Check(modelFound, "ModelView fields are missing model transforms");
  auto configured = pixels(QStringLiteral("configured"));
  auto difference = [&](const bytebuf &a, const bytebuf &b) {
    Check(a.size() == b.size() && !a.empty(), "image readback failed");
    double total = 0;
    for(size_t i = 0; i < a.size(); i++) total += qAbs(int(a[i]) - int(b[i]));
    return total / a.size();
  };
  QJsonObject metrics;
  metrics[QStringLiteral("baseline_configured_mean_byte_difference")] = difference(baseline, configured);
  auto surface = Surface(widget->findChild<QWidget *>(QStringLiteral("render")));
  Drag(viewer, surface);
  auto moved = pixels(QStringLiteral("rotated"));
  metrics[QStringLiteral("configured_rotated_mean_byte_difference")] = difference(configured, moved);
  Check(moved != configured, "rotation did not change OW output");
  viewer->ResetReplayCamera();
  Settled(viewer);
  Check(State(viewer)[QStringLiteral("active_pass_count")].toInt() == 0,
        "reset left recipe passes active");
  auto reset = pixels(QStringLiteral("reset"));
  metrics[QStringLiteral("configured_reset_mean_byte_difference")] = difference(configured, reset);
  WaitIO(viewer);
  auto saved = Read(capture + QStringLiteral(".freecamera.json"));
  Check(QJsonDocument::fromJson(saved).object()[QStringLiteral("version")].toInt() == 3,
        "recipe sidecar did not use version 3");
  automatic->click();
  Pump(50);
  Reload(ctx, capture); // Capture switch while a scan is in flight must ignore stale completion.
  auto restored = State(viewer);
  Check(!restored[QStringLiteral("enabled")].toBool() && restored[QStringLiteral("recipe")] == recipe &&
            restored[QStringLiteral("fields")] == active[QStringLiteral("fields")],
        "recipe/model fields did not restore disabled");
  Check(restored[QStringLiteral("active_pass_count")].toInt() == 0 &&
            restored[QStringLiteral("passes")] == active[QStringLiteral("passes")],
        "reopen did not restore pass form disabled");
  if(passCount)
  {
    QJsonObject invalid = restored;
    invalid[QStringLiteral("enabled")] = true;
    invalid[QStringLiteral("event_id")] = double(lastEvent);
    auto view = invalid[QStringLiteral("view")].toArray();
    view[3] = view[3].toDouble() + 1;
    invalid[QStringLiteral("view")] = view;
    auto rules = invalid[QStringLiteral("passes")].toArray();
    auto rule = rules[0].toObject(); auto colours = rule[QStringLiteral("colours")].toArray();
    auto colour = colours[0].toObject(); colour[QStringLiteral("destination_format")] = 45;
    colours[0] = colour; rule[QStringLiteral("colours")] = colours; rules[0] = rule;
    invalid[QStringLiteral("passes")] = rules;
    configure(invalid);
    for(int i = 0; i < 1000 && State(viewer)[QStringLiteral("pending_update")].toBool(); i++) Pump(10);
    const auto failure = State(viewer);
    Check(!failure[QStringLiteral("last_error")].toString().isEmpty() &&
              !failure[QStringLiteral("last_error")].toString().contains(QStringLiteral("restoring the form failed")) &&
              !failure[QStringLiteral("enabled")].toBool() && failure[QStringLiteral("active_pass_count")].toInt() == 0 &&
              failure[QStringLiteral("view")] == restored[QStringLiteral("view")] &&
              failure[QStringLiteral("passes")] == restored[QStringLiteral("passes")],
          "failed Apply from a reopened capture did not restore its disabled form");
  }
  auto reopened = pixels(QStringLiteral("reopened"));
  metrics[QStringLiteral("baseline_reopened_mean_byte_difference")] = difference(baseline, reopened);
  Write(out + QStringLiteral("/image-metrics.json"), QJsonDocument(metrics).toJson());
  // A bad layout must report failure without replacing the accepted camera configuration.
  auto bad = recipe;
  auto camera = bad[QStringLiteral("camera")].toObject();
  auto locator = camera[QStringLiteral("view")].toObject();
  locator[QStringLiteral("offset")] = 65520;
  camera[QStringLiteral("view")] = locator;
  bad[QStringLiteral("camera")] = camera;
  request[QStringLiteral("recipe")] = bad;
  configure(request);
  Scan(viewer, automatic);
  Check(!State(viewer)[QStringLiteral("last_error")].toString().isEmpty() &&
            State(viewer)[QStringLiteral("fields")] == active[QStringLiteral("fields")],
        "failed recipe changed accepted camera fields");
  Check(Read(capture + QStringLiteral(".freecamera.json")) == saved,
        "failed recipe overwrote saved configuration");
  if(recipe[QStringLiteral("version")].toInt() == 2)
  {
    bad = recipe;
    auto fingerprint = bad[QStringLiteral("capture")].toObject();
    fingerprint[QStringLiteral("sha256")] = QString(64, QChar('0'));
    bad[QStringLiteral("capture")] = fingerprint;
    request[QStringLiteral("recipe")] = bad;
    configure(request);
    Scan(viewer, automatic);
    Check(State(viewer)[QStringLiteral("last_error")].toString().contains(QStringLiteral("fingerprint")) &&
              State(viewer)[QStringLiteral("fields")] == active[QStringLiteral("fields")],
          "wrong capture fingerprint was not rejected");
  }
  request[QStringLiteral("recipe")] = QJsonObject();
  configure(request);
  automatic->click();
  Check(!State(viewer)[QStringLiteral("scanning")].toBool() &&
            State(viewer)[QStringLiteral("scan_report")].toString().contains(QStringLiteral("agent")),
        "empty recipe started a game-specific scan");
  for(int version : {1, 2})
  {
    ctx->CloseCapture();
    auto legacy = QJsonDocument::fromJson(saved).object();
    legacy[QStringLiteral("version")] = version;
    auto configuration = legacy[QStringLiteral("configuration")].toObject();
    configuration.remove(QStringLiteral("passes"));
    configuration.remove(QStringLiteral("recipe"));
    legacy[QStringLiteral("configuration")] = configuration;
    Write(capture + QStringLiteral(".freecamera.json"), QJsonDocument(legacy).toJson());
    Reload(ctx, capture);
    Check(State(viewer)[QStringLiteral("fields")] == active[QStringLiteral("fields")] &&
              !State(viewer)[QStringLiteral("enabled")].toBool() &&
              State(viewer)[QStringLiteral("active_pass_count")].toInt() == 0,
          "legacy sidecar failed to restore disabled");
  }
  ctx->CloseCapture();
  Write(capture + QStringLiteral(".freecamera.json"), saved);
}

static void LargeApplyRun(ICaptureContext *ctx, const QString &out)
{
  Check(QString(ctx->GetCaptureFilename()).startsWith(out), "large Apply requires an owned capture");
  ITextureViewer *viewer = ctx->GetTextureViewer();
  ctx->ShowTextureViewer();
  QJsonObject config = QJsonDocument::fromJson(Read(QString::fromUtf8(qgetenv("REPLAY_CAMERA_CONFIG"))))
                           .object()[QStringLiteral("configuration")].toObject();
  config[QStringLiteral("enabled")] = true;
  config[QStringLiteral("show")] = true;
  int heartbeat = 0;
  QTimer pulse;
  pulse.setInterval(5);
  QObject::connect(&pulse, &QTimer::timeout, [&heartbeat]() { heartbeat++; });
  pulse.start();
  QElapsedTimer timer;
  timer.start();
  Check(viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(config).toJson())).empty(),
        "large camera configuration rejected");
  Check(State(viewer)[QStringLiteral("applying")].toBool(), "large Apply has no immediate progress");
  Settled(viewer);
  pulse.stop();
  auto state = State(viewer);
  state[QStringLiteral("ui_heartbeat_count")] = heartbeat;
  state[QStringLiteral("configure_to_ready_ms")] = double(timer.elapsed());
  Write(out + QStringLiteral("/large-apply.json"), QJsonDocument(state).toJson());
  Check(heartbeat > 2, "large Apply blocked the UI event loop");
  Check(state[QStringLiteral("enabled")].toBool() &&
            !state[QStringLiteral("panel_visible")].toBool(),
        "large Apply did not finish and close the window");
  Check(state[QStringLiteral("event_id")] == config[QStringLiteral("event_id")],
        "large Apply did not publish its target event");
  QWidget *render = viewer->Widget()->findChild<QWidget *>(QStringLiteral("render"));
  Drag(viewer, Surface(render));
  WaitIO(viewer);
  viewer->ResetReplayCamera();
  Settled(viewer);
}

extern "C" __declspec(dllexport) void ScheduleReplayOverrideTest(ICaptureContext *ctx,
                                                                 const char *output)
{
  QString path = QString::fromUtf8(output);
  QTimer *timer = new QTimer(qApp);
  QObject::connect(timer, &QTimer::timeout, [ctx, path, timer]() {
    if(!ctx->IsCaptureLoaded() || ctx->IsCaptureLoading())
      return;
    timer->stop();
    QFile log(path + QStringLiteral("/gui-test.log"));
    log.open(QIODevice::WriteOnly | QIODevice::Truncate);
    try
    {
      if(!qgetenv("REPLAY_CAMERA_RECIPE").isEmpty())
        RecipeRun(ctx, path);
      else if(qgetenv("REPLAY_CAMERA_CONFIG").isEmpty())
        AutoRun(ctx, path);
      else
        LargeApplyRun(ctx, path);
      if(!qgetenv("REPLAY_CAMERA_RECIPE").isEmpty())
        log.write("PASS: recipe store, embedded scan, cancellation, partial approval, Apply progress, "
                  "UI heartbeat, success closes, ModelView, rotation/reset, sidecar v3 camera/pass restore disabled, neutral empty-recipe prompt, fingerprint rejection, "
                  "failed scan preserves configuration\n");
      else if(qgetenv("REPLAY_CAMERA_CONFIG").isEmpty())
        log.write("PASS: native panel, embedded script, cancellation, partial approval, projections, "
                  "movement does not save, reopen disabled, original baseline, optional defaults, "
                  "bad seed, corrupt JSON, version, fingerprint, rename, save failure; both Apply "
                  "progress paths, duplicate protection, success closes, failure stays, UI heartbeat, "
                  "stale Apply completion ignored\n");
      else
        log.write("PASS: large-scene native Apply, immediate progress, UI heartbeat, target event, "
                  "successful close, rotation, save and reset\n");
    }
    catch(const std::exception &error)
    {
      log.write("FAIL: ");
      log.write(error.what());
      log.write("\n");
    }
    log.close();
    ctx->CloseCapture();
    ctx->GetMainWindow()->Widget()->close();
    timer->deleteLater();
  });
  timer->start(100);
}
