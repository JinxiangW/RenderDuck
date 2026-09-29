// Regression for input delivered to the real, replaceable render child. No OS input injection.
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPointer>
#include <QQuaternion>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <stdexcept>
#include "Code/Interface/QRDInterface.h"

static void Check(bool ok, const char *message)
{
  if(!ok)
    throw std::runtime_error(message);
}
static void Pump(int ms)
{
  QElapsedTimer timer;
  timer.start();
  while(timer.elapsed() < ms)
  {
    qApp->processEvents();
    QThread::msleep(5);
  }
}
static QJsonObject State(ITextureViewer *viewer)
{
  return QJsonDocument::fromJson(QString(viewer->GetReplayCameraState()).toUtf8()).object();
}
static QJsonArray Settled(ITextureViewer *viewer)
{
  for(int i = 0; i < 200; i++)
  {
    Pump(10);
    QJsonObject s = State(viewer);
    if(!s[QStringLiteral("busy")].toBool() && !s[QStringLiteral("pending_update")].toBool())
    {
      Check(s[QStringLiteral("last_error")].toString().isEmpty(), "replay reported an error");
      return s[QStringLiteral("current_view")].toArray();
    }
  }
  throw std::runtime_error("camera replay timed out");
}
static QWidget *Surface(QWidget *render)
{
  for(QWidget *w : render->findChildren<QWidget *>())
    if(QByteArray(w->metaObject()->className()) == "CustomPaintWidgetInternal")
      return w;
  throw std::runtime_error("internal render child not found");
}
static void Mouse(QWidget *surface, QEvent::Type type, QPointF point, Qt::MouseButton button,
                  Qt::MouseButtons buttons)
{
  QMouseEvent event(type, point, button, buttons, Qt::NoModifier);
  QApplication::sendEvent(surface, &event);
}
static ResourceId Resource(ICaptureContext *ctx, const char *name)
{
  for(const ResourceDescription &r : ctx->GetResources())
    if(r.name == name)
      return r.resourceId;
  throw std::runtime_error("fixture resource missing");
}
static void Configure(ICaptureContext *ctx)
{
  QJsonArray identity;
  for(int i = 0; i < 16; i++)
    identity.append(i % 5 == 0 ? 1.0 : 0.0);
  QString shader = QVariant::fromValue(Resource(ctx, "SceneVS")).toString();
  Check(!shader.isEmpty(), "ResourceId converter is not initialized");
  QJsonObject field{{QStringLiteral("semantic"), QStringLiteral("View")},
                    {QStringLiteral("shader"), shader},
                    {QStringLiteral("stage"), QStringLiteral("VS")},
                    {QStringLiteral("slot"), 0},
                    {QStringLiteral("offset"), 0},
                    {QStringLiteral("first_event"), 2},
                    {QStringLiteral("last_event"), 35}};
  QJsonObject config{{QStringLiteral("view"), identity},
                     {QStringLiteral("projection"), identity},
                     {QStringLiteral("fields"), QJsonArray{field}},
                     {QStringLiteral("enabled"), true},
                     {QStringLiteral("show"), false},
                     {QStringLiteral("speed"), 1.0},
                     {QStringLiteral("forward"), QStringLiteral("-Z")}};
  ctx->SetEventID({}, 35, 35, true);
  ITextureViewer *viewer = ctx->GetTextureViewer();
  viewer->ViewTexture(Resource(ctx, "Scene"), CompType::Float, true);
  rdcstr error = viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(config).toJson()));
  if(!error.empty())
    throw std::runtime_error(error.c_str());
  Settled(viewer);
  viewer->GotoLocation(32, 32);
}
static void Drag(ITextureViewer *viewer, QWidget *surface)
{
  QJsonArray before = Settled(viewer);
  auto picked = viewer->GetPickedLocation();
  Mouse(surface, QEvent::MouseButtonPress, {20, 20}, Qt::RightButton, Qt::RightButton);
  Mouse(surface, QEvent::MouseMove, {40, 30}, Qt::NoButton, Qt::RightButton);
  Mouse(surface, QEvent::MouseButtonRelease, {40, 30}, Qt::RightButton, Qt::NoButton);
  QJsonArray after = Settled(viewer);
  Check(viewer->GetPickedLocation() == picked,
        "right drag was handled by the pixel picker instead of the camera");
  Check(after != before, "right drag on the internal surface did not rotate the camera");
  Mouse(surface, QEvent::MouseMove, {65, 55}, Qt::NoButton, Qt::NoButton);
  Check(Settled(viewer) == after, "camera moved after right-button release");
}
static QMatrix4x4 Pose(ITextureViewer *viewer)
{
  QJsonArray values = Settled(viewer);
  QMatrix4x4 view;
  for(int i = 0; i < 16; i++)
    view(i / 4, i % 4) = float(values[i].toDouble());
  return view.inverted();
}
static void CheckLevel(const QMatrix4x4 &pose, const QVector3D &position, float handedness,
                       const QVector3D &worldUp = QVector3D(0, 1, 0))
{
  QVector3D right = pose.column(0).toVector3D(), up = pose.column(1).toVector3D(),
            z = pose.column(2).toVector3D();
  Check(qAbs(QVector3D::dotProduct(right, worldUp)) < 0.00001f, "yaw introduced roll: camera right axis is not horizontal");
  Check(QVector3D::dotProduct(up, worldUp) > 0, "camera flipped upside down");
  Check(qAbs(QVector3D::dotProduct(z, worldUp)) <= 0.999848f, "absolute pitch exceeded 89 degrees");
  Check(qAbs(right.length() - 1) < 0.00001f && qAbs(up.length() - 1) < 0.00001f &&
            qAbs(z.length() - 1) < 0.00001f,
        "camera basis lost unit length");
  Check(qAbs(QVector3D::dotProduct(right, up)) < 0.00001f &&
            qAbs(QVector3D::dotProduct(right, z)) < 0.00001f &&
            qAbs(QVector3D::dotProduct(up, z)) < 0.00001f,
        "camera basis lost orthogonality");
  Check(QVector3D::dotProduct(QVector3D::crossProduct(right, up), z) * handedness > .999f,
        "camera handedness changed");
  Check((pose.column(3).toVector3D() - position).length() < .001f,
        "rotation orbited the world origin instead of keeping camera position");
}
static void RotationRegression(ITextureViewer *viewer, QWidget *surface)
{
  for(float handedness : {1.0f, -1.0f})
    for(bool positiveForward : {false, true})
    {
      QMatrix4x4 initial;
      initial.translate(3, 5, -7);
      initial.rotate(37, 0, 1, 0);
      initial.rotate(30, 1, 0, 0);
      initial.scale(handedness, 1, 1);
      QJsonObject config = State(viewer);
      QJsonArray matrix;
      QMatrix4x4 view = initial.inverted();
      for(int i = 0; i < 16; i++)
        matrix.append(view(i / 4, i % 4));
      config[QStringLiteral("view")] = matrix;
      config[QStringLiteral("enabled")] = true;
      config[QStringLiteral("show")] = false;
      config[QStringLiteral("forward")] =
          positiveForward ? QStringLiteral("+Z") : QStringLiteral("-Z");
      Check(viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(config).toJson())).empty(),
            "pitched camera configuration failed");
      QMatrix4x4 start = Pose(viewer);
      QVector3D position = start.column(3).toVector3D();
      float elevation = start(1, 2);
      for(int step = 0; step < 9; step++)
      {
        Mouse(surface, QEvent::MouseButtonPress, {20, 20}, Qt::RightButton, Qt::RightButton);
        Mouse(surface, QEvent::MouseMove, {220, 20}, Qt::NoButton, Qt::RightButton);
        Mouse(surface, QEvent::MouseButtonRelease, {220, 20}, Qt::RightButton, Qt::NoButton);
        QMatrix4x4 pose = Pose(viewer);
        CheckLevel(pose, position, handedness);
        Check(qAbs(pose(1, 2) - elevation) < .00001f, "horizontal yaw changed camera elevation");
        if(step == 0)
        {
          float f = positiveForward ? 1.0f : -1.0f;
          Check(QVector3D::dotProduct(pose.column(2).toVector3D() * f,
                                      start.column(0).toVector3D()) > 0,
                "right drag turned opposite to camera right");
        }
      }
      Check((Pose(viewer).column(2).toVector3D() - start.column(2).toVector3D()).length() < .00001f,
            "full yaw turn did not return to starting direction");
      for(float dy : {-3000.0f, 6000.0f})
      {
        Mouse(surface, QEvent::MouseButtonPress, {20, 20}, Qt::RightButton, Qt::RightButton);
        Mouse(surface, QEvent::MouseMove, {70, 20 + dy}, Qt::NoButton, Qt::RightButton);
        Mouse(surface, QEvent::MouseButtonRelease, {70, 20 + dy}, Qt::RightButton, Qt::NoButton);
        QMatrix4x4 pose = Pose(viewer);
        CheckLevel(pose, position, handedness);
        Check(qAbs(qAbs(pose(1, 2)) - .9998477f) < .00001f,
              "pitch limit was applied to a delta instead of world elevation");
      }
    }

  // Existing roll is removed on activation; vertical captures retain a stable heading
  // while being brought just inside the pitch limit.
  for(float elevation : {-90.0f, 35.0f, 90.0f})
  {
    QMatrix4x4 initial;
    initial.translate(3, 5, -7);
    initial.rotate(20, 0, 1, 0);
    initial.rotate(elevation, 1, 0, 0);
    initial.rotate(27, 0, 0, 1);
    initial.scale(-1, 1, 1);
    QJsonObject config = State(viewer);
    QJsonArray values;
    QMatrix4x4 view = initial.inverted();
    for(int i = 0; i < 16; i++)
      values.append(view(i / 4, i % 4));
    config[QStringLiteral("view")] = values;
    config[QStringLiteral("show")] = false;
    Check(viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(config).toJson())).empty(),
          "rolled camera configuration failed");
    QMatrix4x4 pose = Pose(viewer);
    CheckLevel(pose, initial.column(3).toVector3D(), -1);
    Check(QVector3D::dotProduct(pose.column(2).toVector3D(), initial.column(2).toVector3D()) > .9997f,
          "removing roll changed the view direction");
  }
}
static void HorizontalProjectionRegression(ITextureViewer *viewer, QWidget *render, QWidget *surface)
{
  // Judge direction in projected screen space, not by assuming camera +X is screen right.
  for(float handedness : {-1.0f, 1.0f})
    for(float forward : {-1.0f, 1.0f})
      for(float horizontal : {-1.0f, 1.0f})
        for(float homogeneous : {-1.0f, 1.0f})
        {
          QMatrix4x4 pose;
          pose.translate(3, 5, -7);
          pose.rotate(37, 0, 1, 0);
          pose.rotate(20, 1, 0, 0);
          pose.scale(handedness, 1, 1);
          QMatrix4x4 projection;
          projection(0, 0) = horizontal * .8f;
          projection(1, 1) = 1.4f;
          projection(2, 2) = -.000006f;
          projection(2, 3) = .1f;
          projection(3, 2) = forward;
          projection(3, 3) = 0;
          projection *= homogeneous;
          QJsonObject config = State(viewer);
          QJsonArray v, p;
          const QMatrix4x4 view = pose.inverted();
          for(int i = 0; i < 16; i++) { v.append(view(i / 4, i % 4)); p.append(projection(i / 4, i % 4)); }
          config[QStringLiteral("view")] = v;
          config[QStringLiteral("projection")] = p;
          auto viewField = config[QStringLiteral("fields")].toArray()[0].toObject();
          auto projectionField = viewField;
          projectionField[QStringLiteral("semantic")] = QStringLiteral("Projection");
          projectionField[QStringLiteral("offset")] = 80;
          config[QStringLiteral("fields")] = QJsonArray{viewField, projectionField};
          config[QStringLiteral("forward")] = forward < 0 ? QStringLiteral("-Z") : QStringLiteral("+Z");
          config[QStringLiteral("enabled")] = true;
          config[QStringLiteral("show")] = false;
          config[QStringLiteral("speed")] = 1;
          Check(viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(config).toJson())).empty(),
                "projection direction configuration failed");
          Settled(viewer);
          render->window()->activateWindow(); render->setFocus(); Pump(30);
          Check(render->hasFocus(), "direction test has no keyboard focus");
          const int inputs[] = {Qt::Key_D, Qt::Key_A, Qt::Key_Right, Qt::Key_Left, 0, 1};
          for(int key : inputs)
          {
            QMatrix4x4 before = Pose(viewer);
            const QVector4D world = before * QVector4D(0, 0, forward * 10, 1);
            const QVector4D oldClip = projection * before.inverted() * world;
            if(key > 1)
            {
              QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier), release(QEvent::KeyRelease, key, Qt::NoModifier);
              QApplication::sendEvent(surface, &press); Pump(80); QApplication::sendEvent(surface, &release);
            }
            else
            {
              const int dx = key == 0 ? 20 : -20;
              Mouse(surface, QEvent::MouseButtonPress, {40, 40}, Qt::RightButton, Qt::RightButton);
              Mouse(surface, QEvent::MouseMove, {40.0 + dx, 40}, Qt::NoButton, Qt::RightButton);
              Mouse(surface, QEvent::MouseButtonRelease, {40.0 + dx, 40}, Qt::RightButton, Qt::NoButton);
            }
            QMatrix4x4 after = Pose(viewer);
            const QVector4D clip = projection * after.inverted() * world;
            const float screenDelta = clip.x() / clip.w() - oldClip.x() / oldClip.w();
            const bool right = key == Qt::Key_D || key == Qt::Key_Right || key == 0;
            Check(right ? screenDelta < -.0001f : screenDelta > .0001f,
                  "horizontal input is reversed in the projected image");
            CheckLevel(after, after.column(3).toVector3D(), handedness);
          }
        }
}

static void WorldUpRegression(ICaptureContext *ctx, ITextureViewer *viewer, QWidget *render, QWidget *surface)
{
  for(const QString &axis : {QStringLiteral("+Y"), QStringLiteral("-Y"), QStringLiteral("+X"),
                             QStringLiteral("-X"), QStringLiteral("-Z"), QStringLiteral("+Z")})
    for(float forward : {-1.0f, 1.0f})
    {
      QVector3D up;
      up[axis[1] == QChar('X') ? 0 : axis[1] == QChar('Y') ? 1 : 2] = axis[0] == QChar('-') ? -1 : 1;
      QMatrix4x4 pose;
      pose.translate(3, 5, -7);
      pose.rotate(QQuaternion::rotationTo(QVector3D(0, 1, 0), up));
      pose.rotate(23, 0, 1, 0); pose.rotate(25, 1, 0, 0); pose.scale(-1, 1, 1);
      QMatrix4x4 projection;
      projection(0, 0) = -.8f; projection(1, 1) = 1.4f;
      projection(2, 2) = 0; projection(2, 3) = .1f;
      projection(3, 2) = forward; projection(3, 3) = 0;
      QJsonArray v, p;
      const QMatrix4x4 view = pose.inverted();
      for(int i = 0; i < 16; i++) { v.append(view(i / 4, i % 4)); p.append(projection(i / 4, i % 4)); }
      auto config = State(viewer);
      config[QStringLiteral("view")] = v; config[QStringLiteral("projection")] = p;
      config[QStringLiteral("world_up")] = axis;
      config[QStringLiteral("forward")] = forward < 0 ? QStringLiteral("-Z") : QStringLiteral("+Z");
      config[QStringLiteral("show")] = false; config[QStringLiteral("enabled")] = true;
      Check(viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(config).toJson())).empty(), "world up rejected");
      Settled(viewer); render->window()->activateWindow(); render->setFocus(); Pump(30);
      auto initial = Pose(viewer);
      Check(QVector3D::dotProduct(initial.column(2).toVector3D(), pose.column(2).toVector3D()) > .99999f,
            "world-up selection changed the captured viewing direction");
      CheckLevel(initial, pose.column(3).toVector3D(), -1, up);
      const int keys[] = {Qt::Key_Q, Qt::Key_E, Qt::Key_W, Qt::Key_D, Qt::Key_Right};
      for(int key : keys)
      {
        auto before = Pose(viewer);
        const QVector4D world = before * QVector4D(0, 0, forward * 10, 1);
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier), release(QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(surface, &press); Pump(80); QApplication::sendEvent(surface, &release);
        auto after = Pose(viewer);
        const QVector3D delta = after.column(3).toVector3D() - before.column(3).toVector3D();
        if(key == Qt::Key_Q || key == Qt::Key_E)
        {
          const float distance = QVector3D::dotProduct(delta, up);
          Check(key == Qt::Key_E ? distance > .01f : distance < -.01f, "Q/E ignored configured world up");
          Check((delta - up * distance).length() < .0001f, "Q/E moved sideways");
        }
        if(key == Qt::Key_W)
          Check(QVector3D::dotProduct(delta, before.column(2).toVector3D() * forward) > .01f,
                "W ignored camera-local forward");
        if(key == Qt::Key_D || key == Qt::Key_Right)
        {
          const QVector4D clip = projection * after.inverted() * world;
          Check(clip.x() / clip.w() < -.0001f, "world-up controls turned/moved opposite to screen right");
        }
        if(key == Qt::Key_Right)
        {
          CheckLevel(after, before.column(3).toVector3D(), -1, up);
          Check(qAbs(QVector3D::dotProduct(after.column(2).toVector3D() - before.column(2).toVector3D(), up)) < .00001f,
                "yaw changed world-relative elevation");
        }
      }
      const auto before = Pose(viewer);
      Mouse(surface, QEvent::MouseButtonPress, {40,40}, Qt::RightButton, Qt::RightButton);
      Mouse(surface, QEvent::MouseMove, {60,3040}, Qt::NoButton, Qt::RightButton);
      Mouse(surface, QEvent::MouseButtonRelease, {60,3040}, Qt::RightButton, Qt::NoButton);
      const auto after = Pose(viewer);
      CheckLevel(after, before.column(3).toVector3D(), -1, up);
      Check(qAbs(qAbs(QVector3D::dotProduct(after.column(2).toVector3D(), up)) - .9998477f) < .00001f,
            "world-relative pitch limit failed");
    }
  for(int i = 0; i < 400 && State(viewer)[QStringLiteral("saving_configuration")].toBool(); i++) Pump(20);
  rdcstr capture = ctx->GetCaptureFilename();
  ctx->CloseCapture();
  ctx->LoadCapture(capture, ReplayOptions(), capture, false, true);
  for(int i = 0; i < 400; i++)
  {
    Pump(20);
    if(!State(viewer)[QStringLiteral("saving_configuration")].toBool() &&
       State(viewer)[QStringLiteral("world_up")].toString() == QStringLiteral("+Z")) break;
  }
  Check(State(viewer)[QStringLiteral("world_up")].toString() == QStringLiteral("+Z") &&
            !State(viewer)[QStringLiteral("enabled")].toBool(), "world up did not restore disabled");
  auto invalid = State(viewer); invalid[QStringLiteral("world_up")] = QStringLiteral("invalid");
  Check(!viewer->ConfigureReplayCamera(QString::fromUtf8(QJsonDocument(invalid).toJson())).empty(), "invalid world up accepted");
  Configure(ctx); // An absent world_up must reset to the legacy +Y default.
  Check(State(viewer)[QStringLiteral("world_up")].toString() == QStringLiteral("+Y"), "legacy world-up default changed");
}

static void Run(ICaptureContext *ctx)
{
  Check(QString(ctx->GetCaptureFilename()).contains(QStringLiteral("fixture-")),
        "not an owned fixture");
  rdcstr capture = ctx->GetCaptureFilename();
  Configure(ctx);
  ITextureViewer *viewer = ctx->GetTextureViewer();
  QWidget *render = viewer->Widget()->findChild<QWidget *>(QStringLiteral("render"));
  Check(render != nullptr, "render widget not found");
  QWidget *surface = Surface(render);
  Drag(viewer, surface);

  // A lost release / capture must never turn ordinary hovering into camera motion or picking.
  Mouse(surface, QEvent::MouseButtonPress, {20, 20}, Qt::RightButton, Qt::RightButton);
  Mouse(surface, QEvent::MouseMove, {25, 25}, Qt::NoButton, Qt::RightButton);
  Settled(viewer);
  Mouse(surface, QEvent::MouseMove, {30, 30}, Qt::NoButton, Qt::NoButton);
  QJsonArray stopped = Settled(viewer);
  auto picked = viewer->GetPickedLocation();
  Mouse(surface, QEvent::MouseMove, {45, 45}, Qt::NoButton, Qt::RightButton);
  Check(Settled(viewer) == stopped, "missing release left dragging active");
  Check(viewer->GetPickedLocation() == picked, "orphaned drag reached pixel picker");

  Mouse(surface, QEvent::MouseButtonPress, {20, 20}, Qt::RightButton, Qt::RightButton);
  QFocusEvent lost(QEvent::FocusOut);
  QApplication::sendEvent(render, &lost);
  Mouse(surface, QEvent::MouseMove, {40, 40}, Qt::NoButton, Qt::RightButton);
  Check(Settled(viewer) == stopped, "focus loss did not cancel dragging");
  Check(viewer->GetPickedLocation() == picked, "focus-loss drag reached pixel picker");

  Mouse(surface, QEvent::MouseButtonPress, {20, 20}, Qt::RightButton, Qt::RightButton);
  QEvent ungrab(QEvent::UngrabMouse);
  QApplication::sendEvent(surface, &ungrab);
  Mouse(surface, QEvent::MouseMove, {45, 45}, Qt::NoButton, Qt::RightButton);
  Check(Settled(viewer) == stopped, "lost mouse grab did not cancel dragging");
  Check(viewer->GetPickedLocation() == picked, "lost-grab drag reached pixel picker");

  Mouse(surface, QEvent::MouseButtonDblClick, {20, 20}, Qt::RightButton, Qt::RightButton);
  Mouse(surface, QEvent::MouseMove, {40, 30}, Qt::NoButton, Qt::RightButton);
  Mouse(surface, QEvent::MouseButtonRelease, {40, 30}, Qt::RightButton, Qt::NoButton);
  Check(Settled(viewer) != stopped, "right double-click drag did not rotate");
  Check(viewer->GetPickedLocation() == picked, "double-click drag reached pixel picker");

  QPointer<QWidget> oldSurface = surface;
  Check(ctx->GetFatalError().OK(), "replay became fatal before capture close");
  ctx->CloseCapture();
  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  Pump(20);
  Check(oldSurface.isNull(), "capture close did not recreate render surface");
  ctx->LoadCapture(capture, ReplayOptions(), capture, false, true);
  Check(ctx->IsCaptureLoaded(), "fixture reload failed");
  Configure(ctx);
  surface = Surface(render);
  Drag(viewer, surface);

  RotationRegression(viewer, surface);
  HorizontalProjectionRegression(viewer, render, surface);
  WorldUpRegression(ctx, viewer, render, surface);
  surface = Surface(render);

  // Exercise the actual timer path, including acceleration and key cancellation.
  render->window()->show();
  render->window()->activateWindow();
  render->setFocus();
  Pump(100);
  Check(render->hasFocus(), "keyboard fixture could not focus the render widget");
  auto key = [surface](int k, bool down) {
    QKeyEvent event(down ? QEvent::KeyPress : QEvent::KeyRelease, k, Qt::NoModifier);
    QApplication::sendEvent(surface, &event);
  };
  float y = Pose(viewer)(1, 3);
  key(Qt::Key_Q, true);
  Pump(160);
  key(Qt::Key_Q, false);
  float down = y - Pose(viewer)(1, 3);
  Check(down > 0.01f, "Q did not move down");
  y = Pose(viewer)(1, 3);
  key(Qt::Key_E, true);
  Pump(160);
  key(Qt::Key_E, false);
  Check(Pose(viewer)(1, 3) > y + 0.01f, "E did not move up");
  y = Pose(viewer)(1, 3);
  key(Qt::Key_Shift, true);
  key(Qt::Key_E, true);
  Pump(160);
  key(Qt::Key_E, false);
  key(Qt::Key_Shift, false);
  Check(Pose(viewer)(1, 3) - y > down * 2, "Shift did not accelerate movement");
  key(Qt::Key_E, true);
  QApplication::sendEvent(render, &lost);
  QJsonArray cancelled = Settled(viewer);
  Pump(100);
  Check(Settled(viewer) == cancelled, "focus loss did not cancel keyboard movement");

  viewer->ResetReplayCamera();
  Settled(viewer);
  viewer->GotoLocation(32, 32);
  picked = viewer->GetPickedLocation();
  Mouse(surface, QEvent::MouseButtonPress, {0, 0}, Qt::RightButton, Qt::RightButton);
  Mouse(surface, QEvent::MouseMove, {1, 1}, Qt::NoButton, Qt::RightButton);
  Mouse(surface, QEvent::MouseButtonRelease, {1, 1}, Qt::RightButton, Qt::NoButton);
  Pump(30);
  Check(!(viewer->GetPickedLocation() == picked), "disabling camera did not restore pixel picking");
}
extern "C" __declspec(dllexport) void ScheduleReplayOverrideTest(ICaptureContext *ctx,
                                                                 const char *output)
{
  QString path = QString::fromUtf8(output);
  QTimer *timer = new QTimer(qApp);
  int *attempts = new int(0);
  QObject::connect(timer, &QTimer::timeout, [ctx, path, timer, attempts]() {
    if((!ctx->IsCaptureLoaded() || ctx->IsCaptureLoading()) && ++*attempts < 100)
      return;
    timer->stop();
    delete attempts;
    QFile log(path + QStringLiteral("/gui-test.log"));
    log.open(QIODevice::WriteOnly | QIODevice::Truncate);
    try
    {
      Check(ctx->IsCaptureLoaded() && !ctx->IsCaptureLoading(), "capture did not finish loading");
      Run(ctx);
      log.write(
          "PASS: internal-child drag, no pixel picking, release, lost release, focus loss, "
          "recreated child, disabled-mode picking; level yaw/pitch, absolute pitch clamp, "
          "orthonormal basis, handedness, +/-Z and stationary camera position; Q down, E up, Shift "
          "and keyboard focus loss; projected left/right movement, arrows and drag with mirrored "
          "projection, both handedness/forward conventions and homogeneous sign; all six world-up "
          "axes, world-relative yaw/pitch/QE, local forward, sidecar restore and legacy default\n");
    }
    catch(const std::exception &e)
    {
      log.write("FAIL: ");
      log.write(e.what());
      log.write("\n");
    }
    log.close();
    ctx->CloseCapture();
    ctx->GetMainWindow()->Widget()->close();
    timer->deleteLater();
  });
  timer->start(100);
}
