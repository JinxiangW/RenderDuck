// In-process Qt interaction test, loaded only by gui_test.py in a separate test instance.
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFocusEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QAbstractItemDelegate>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <stdexcept>
#include "Code/Interface/QRDInterface.h"

static void Require(bool condition, const char *message)
{
  if(!condition)
    throw std::runtime_error(message);
}
static void Wait(int ms)
{
  QElapsedTimer t;
  t.start();
  while(t.elapsed() < ms)
  {
    qApp->processEvents();
    QThread::msleep(5);
  }
}
static void Ready(QWidget *root)
{
  QElapsedTimer t;
  t.start();
  QLabel *state = root->findChild<QLabel *>(QStringLiteral("replayCameraState"));
  Require(state != nullptr, "camera state missing");
  do
  {
    Wait(30);
  } while(state->text() != QStringLiteral("Camera active") &&
          state->text() != QStringLiteral("Original camera") && t.elapsed() < 10000);
  Require(state->text() == QStringLiteral("Camera active") ||
              state->text() == QStringLiteral("Original camera"), "camera replay did not finish");
}
static QPushButton *Button(QWidget *root, const char *label)
{
  for(QPushButton *b : root->findChildren<QPushButton *>())
    if(b->text() == QString::fromUtf8(label))
      return b;
  throw std::runtime_error(label);
}
static const ActionDescription *Marker(const rdcarray<ActionDescription> &actions, const char *name)
{
  for(const ActionDescription &a : actions)
  {
    if(a.customName == name)
      return &a;
    if(auto *found = Marker(a.children, name))
      return found;
  }
  return nullptr;
}
static uint32_t Last(const ActionDescription *a)
{
  uint32_t end = a->eventId;
  for(const ActionDescription &child : a->children)
    end = qMax(end, Last(&child));
  return end;
}
static ResourceId Resource(ICaptureContext *ctx, const char *name)
{
  for(const ResourceDescription &r : ctx->GetResources())
    if(r.name == name)
      return r.resourceId;
  throw std::runtime_error(name);
}
static bytebuf Pixels(ICaptureContext *ctx, const char *name)
{
  bytebuf b;
  ResourceId id = Resource(ctx, name);
  ctx->Replay().BlockInvoke([&](IReplayController *r) { b = r->GetTextureData(id, Subresource()); });
  return b;
}
static void Key(QWidget *widget, QEvent::Type type, int key)
{
  QKeyEvent e(type, key, Qt::NoModifier);
  QApplication::sendEvent(widget, &e);
}
static void SetChoice(QTableWidget *table, int row, int column, int value)
{
  const QModelIndex index = table->model()->index(row, column);
  QStyleOptionViewItem option;
  auto delegate = table->itemDelegate(index);
  QWidget *editor = delegate->createEditor(table->viewport(), option, index);
  auto combo = qobject_cast<QComboBox *>(editor);
  Require(combo != nullptr, "camera choice delegate did not create a combobox");
  delegate->setEditorData(editor, index);
  combo->setCurrentIndex(value);
  delegate->setModelData(editor, table->model(), index);
  delete editor;
}
static QWidget *MouseSurface(QWidget *render)
{
  // Real mouse input targets the native child, not the persistent outer widget.
  for(QWidget *child : render->findChildren<QWidget *>())
    if(QByteArray(child->metaObject()->className()) == "CustomPaintWidgetInternal") return child;
  throw std::runtime_error("internal render surface missing");
}
static bool HasLabel(QAbstractItemModel *model, QModelIndex parent = QModelIndex())
{
  for(int i = 0; i < model->rowCount(parent); i++)
  {
    for(int c = 0; c < model->columnCount(parent); c++)
      if(model->index(i, c, parent).data().toString().startsWith(QStringLiteral("[Bypass]")))
        return true;
    if(HasLabel(model, model->index(i, 0, parent)))
      return true;
  }
  return false;
}
static void Run(ICaptureContext *ctx, const QString &out)
{
  ctx->ShowEventBrowser();
  Wait(100);
  const ActionDescription *scene = Marker(ctx->CurRootActions(), "Scene");
  Require(scene != nullptr, "scene marker missing");
  uint32_t sceneEnd = Last(scene);
  const ActionDescription *draw = nullptr;
  for(const ActionDescription &a : scene->children)
    if(a.flags & ActionFlags::Drawcall)
    {
      draw = &a;
      break;
    }
  Require(draw != nullptr, "scene draw missing");
  ctx->SetEventID({}, draw->eventId, draw->eventId, true);
  QWidget *root = ctx->GetTextureViewer()->Widget();
  ctx->GetTextureViewer()->ViewTexture(Resource(ctx, "Scene"), CompType::Float, true);
  Wait(100);
  Button(root, "Free camera...")->click();
  Wait(50);
  QTableWidget *table = root->findChild<QTableWidget *>(QStringLiteral("replayCameraFields"));
  Require(table != nullptr, "camera table missing");
  QWidget *dialog = table->window();
  // Exercise both reflection pickers using their real modal dialogs.
  for(const QString field : {QStringLiteral("View"), QStringLiteral("Projection")})
  {
    QTimer accept;
    accept.setInterval(25);
    QObject::connect(&accept, &QTimer::timeout, [field]() {
      if(QInputDialog *d = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
      {
        QComboBox *box = d->findChild<QComboBox *>();
        if(box)
          for(int i = 0; i < box->count(); i++)
            if(box->itemText(i).endsWith(QStringLiteral(".") + field))
            {
              box->setCurrentIndex(i);
              break;
            }
        d->accept();
      }
    });
    accept.start();
    Button(dialog, "From reflection...")->click();
    accept.stop();
  }
  Require(table->rowCount() == 2, "reflection did not populate two camera fields");
  SetChoice(table, 1, 0, 1);
  for(int r = 0; r < 2; r++)
  {
    table->item(r, 1)->setText(QString::number(scene->eventId));
    table->item(r, 2)->setText(QString::number(sceneEnd));
  }
  Button(dialog, "Read View / Projection")->click();
  QLabel *status = dialog->findChild<QLabel *>(QStringLiteral("replayOverrideStatus"));
  Require(status->text().startsWith(QStringLiteral("Read base")),
          "baseline reflection read failed");
  ctx->SetEventID({}, sceneEnd, sceneEnd, true);
  Wait(100);
  bytebuf baseline = Pixels(ctx, "Scene");
  Require(!baseline.empty(), "empty GUI baseline");
  Button(dialog, "Apply and enable camera")->click();
  Ready(root);
  Require(Pixels(ctx, "Scene") == baseline, "identity camera changed image");
  Require(dialog->grab().save(out + QStringLiteral("/camera-panel.png")),
          "camera screenshot failed");
  dialog->hide();
  QWidget *render = root->findChild<QWidget *>(QStringLiteral("render"));
  Require(render != nullptr, "render widget missing");
  root->window()->activateWindow();
  render->setFocus();
  Wait(100);
  Require(render->hasFocus(), "render focus missing");
  Key(render, QEvent::KeyPress, Qt::Key_D);
  Wait(220);
  Key(render, QEvent::KeyRelease, Qt::Key_D);
  Ready(root);
  Require(Pixels(ctx, "Scene") != baseline, "WASD did not move camera");
  // Losing focus must clear the held direction, even without a key release.
  Key(render, QEvent::KeyPress, Qt::Key_A);
  Wait(60);
  QFocusEvent lost(QEvent::FocusOut);
  QApplication::sendEvent(render, &lost);
  Ready(root);
  bytebuf stopped = Pixels(ctx, "Scene");
  Wait(150);
  Require(Pixels(ctx, "Scene") == stopped, "camera continued after focus loss");
  Key(render, QEvent::KeyPress, Qt::Key_Right);
  Wait(140);
  Key(render, QEvent::KeyRelease, Qt::Key_Right);
  Ready(root);
  Require(Pixels(ctx, "Scene") != stopped, "arrow rotation did not change image");
  Button(root, "Reset view")->click();
  Ready(root);
  Require(Pixels(ctx, "Scene") == baseline, "reset did not restore baseline");
  Require(ctx->CurEvent() == sceneEnd, "camera arrow changed selected event");
  Button(root, "Enable")->click();
  Ready(root);
  render->setFocus();
  Key(render, QEvent::KeyPress, Qt::Key_Q);
  Wait(100);
  Key(render, QEvent::KeyRelease, Qt::Key_Q);
  Ready(root);
  Require(Pixels(ctx, "Scene") != baseline, "vertical movement did not change image");
  bytebuf raised = Pixels(ctx, "Scene");
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(20, 20), Qt::RightButton, Qt::RightButton,
                    Qt::NoModifier);
  QMouseEvent move(QEvent::MouseMove, QPointF(40, 30), Qt::NoButton, Qt::RightButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(40, 30), Qt::RightButton, Qt::NoButton,
                      Qt::NoModifier);
  QWidget *surface = MouseSurface(render);
  auto picked = ctx->GetTextureViewer()->GetPickedLocation();
  QApplication::sendEvent(surface, &press);
  QApplication::sendEvent(surface, &move);
  QApplication::sendEvent(surface, &release);
  Ready(root);
  Require(ctx->GetTextureViewer()->GetPickedLocation() == picked,
          "free camera right drag reached the pixel picker");
  Require(Pixels(ctx, "Scene") != raised, "mouse rotation did not change image");
  Button(root, "Reset view")->click();
  Ready(root);
  Require(Pixels(ctx, "Scene") == baseline, "second reset failed");

  const ActionDescription *ref = Marker(ctx->CurRootActions(), "Reference");
  ctx->SetEventID({}, Last(ref), Last(ref), true);
  bytebuf reference = Pixels(ctx, "Reference");
  bytebuf referenceDepth = Pixels(ctx, "ReferenceDepth");
  ctx->SetEventID({}, sceneEnd, sceneEnd, true);
  dialog->findChild<QPlainTextEdit *>(QStringLiteral("replayBaseView"))
      ->setPlainText(QStringLiteral("1 0 0 .35\n0 1 0 0\n0 0 1 0\n0 0 0 1"));
  for(int row = 0; row < 2; row++)
  {
    SetChoice(table, row, 7, 1);
    SetChoice(table, row, 8, 1);
  }
  Button(root, "Enable")->click();
  Ready(root);
  Require(Pixels(ctx, "Scene") == reference, "storage and multiplication conversion failed");
  Button(root, "Reset view")->click();
  Ready(root);
  Require(Pixels(ctx, "Scene") == baseline, "convention test reset failed");

  // ModelView must preserve a nonidentity object transform while changing the camera.
  // .15 camera translation plus .20 object translation matches the direct .35 reference.
  SetChoice(table, 0, 0, 9);
  table->item(0, 10)->setText(QStringLiteral("1 0 0 .20\n0 1 0 0\n0 0 1 0\n0 0 0 1"));
  dialog->findChild<QPlainTextEdit *>(QStringLiteral("replayBaseView"))
      ->setPlainText(QStringLiteral("1 0 0 .15\n0 1 0 0\n0 0 1 0\n0 0 0 1"));
  Button(root, "Enable")->click();
  Ready(root);
  Require(Pixels(ctx, "Scene") == reference && Pixels(ctx, "SceneDepth") == referenceDepth,
          "ModelView colour/depth differs from independent engine reference");
  Button(root, "Reset view")->click();
  Ready(root);
  Require(Pixels(ctx, "Scene") == baseline, "ModelView reset failed");

  Button(root, "Free camera...")->click();
  dialog->findChild<QTabWidget *>(QStringLiteral("overrideTabs"))->setCurrentIndex(1);
  Button(dialog, "Add range")->click();
  QTableWidget *passes = dialog->findChild<QTableWidget *>(QStringLiteral("replayPassRules"));
  Require(passes && passes->rowCount() == 1, "pass row missing");
  const ActionDescription *taa = Marker(ctx->CurRootActions(), "TAA_PS");
  passes->item(0, 0)->setText(QString::number(taa->eventId));
  passes->item(0, 1)->setText(QString::number(Last(taa)));
  ((QComboBox *)passes->cellWidget(0, 2))->setCurrentIndex(2);
  for(auto entry : {qMakePair(3, (const char *)"Scene"), qMakePair(7, (const char *)"TAA_PS")})
  {
    QComboBox *box = (QComboBox *)passes->cellWidget(0, entry.first);
    box->setCurrentIndex(box->findData(QVariant::fromValue(Resource(ctx, entry.second))));
  }
  Button(dialog, "Apply passes")->click();
  Require(status->text().startsWith(QStringLiteral("Applied")), "GUI pass apply failed");
  ctx->SetEventID({}, Last(taa), Last(taa), true);
  Require(Pixels(ctx, "TAA_PS") == baseline, "GUI bypass image differs");
  Require(dialog->grab().save(out + QStringLiteral("/pass-panel.png")), "pass screenshot failed");
  QTreeView *events =
      ctx->GetEventBrowser()->Widget()->findChild<QTreeView *>(QStringLiteral("events"));
  for(int i = 0; events && !HasLabel(events->model()) && i < 100; i++) Wait(10);
  if(events && !HasLabel(events->model()))
  {
    ctx->GetEventBrowser()->Widget()->grab().save(out + QStringLiteral("/event-label-failure.png"));
    QFile dump(out + QStringLiteral("/event-label-failure.txt"));
    dump.open(QIODevice::WriteOnly | QIODevice::Truncate);
    std::function<void(QModelIndex)> walk = [&](QModelIndex parent) {
      for(int row = 0; row < events->model()->rowCount(parent); row++)
      {
        for(int col = 0; col < events->model()->columnCount(parent); col++)
          dump.write(events->model()->index(row, col, parent).data().toString().toUtf8() + " | ");
        dump.write("\n");
        walk(events->model()->index(row, 0, parent));
      }
    };
    walk(QModelIndex());
  }
  Require(events && HasLabel(events->model()), "event browser bypass marker missing");
  Button(dialog, "Restore all passes")->click();
  ctx->SetEventID({}, Last(taa), Last(taa), true);
  Require(Pixels(ctx, "TAA_PS") != baseline, "restore passes did not re-enable TAA");
  ctx->CloseCapture();
  Require(table->rowCount() == 0 && passes->rowCount() == 0, "capture close retained rules");
}

extern "C" __declspec(dllexport) void ScheduleReplayOverrideTest(ICaptureContext *ctx,
                                                                 const char *output)
{
  QString out = QString::fromUtf8(output);
  QTimer *timer = new QTimer(qApp);
  int *attempts = new int(0);
  QObject::connect(timer, &QTimer::timeout, [ctx, timer, attempts, out]() {
    if((!ctx->IsCaptureLoaded() || ctx->IsCaptureLoading()) && ++*attempts < 100)
      return;
    timer->stop();
    delete attempts;
    QFile log(out + QStringLiteral("/gui-test.log"));
    log.open(QIODevice::WriteOnly | QIODevice::Truncate);
    try
    {
      Require(ctx->IsCaptureLoaded(), "capture did not load");
      Run(ctx, out);
      log.write(
          "PASS: reflection, matrix read, keyboard movement, focus loss, rotation, reset, pass "
          "editor, event labels, ModelView engine-reference colour/depth, restore and close\n");
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
