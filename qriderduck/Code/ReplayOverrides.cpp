#include "ReplayOverrides.h"
#include <QCheckBox>
#include <QChildEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <atomic>
#include <cmath>
#include "pyriderduck/PythonContext.h"
#include "CaptureContext.h"
#include "QRDUtils.h"

QMap<ICaptureContext *, ReplayOverrides *> ReplayOverrides::s_Editors;
struct CameraScan
{
  PythonContext *python = new PythonContext();
  std::atomic<bool> cancelled{false};
  bool started = false;
  QString capturePath;
  QJsonObject fingerprint;
  QJsonObject recipe;
  ~CameraScan() { python->Finish(); }
};
namespace
{
const QStringList Semantics = {lit("View"),
                               lit("Projection"),
                               lit("ViewProjection"),
                               lit("InverseView"),
                               lit("InverseProjection"),
                               lit("InverseViewProjection"),
                               lit("CameraPosition"),
                               lit("RelativeViewProjection"),
                               lit("InverseRelativeViewProjection"), lit("ModelView")};
const QStringList Stages = {lit("VS"), lit("HS"), lit("DS"), lit("GS"), lit("PS"), lit("CS")};
const QStringList WorldUpAxes = {lit("+Y"), lit("-Y"), lit("+Z"), lit("-Z"), lit("+X"), lit("-X")};
QComboBox *Choice(QTableWidget *table, int row, int col, const QStringList &values, int selected)
{
  if(table->property("delegateChoices").toBool())
  {
    QTableWidgetItem *item = new QTableWidgetItem(values[selected]);
    item->setData(Qt::UserRole, selected);
    item->setData(Qt::UserRole + 1, values);
    table->setItem(row, col, item);
    return NULL;
  }
  QComboBox *box = new QComboBox(table);
  box->addItems(values);
  box->setCurrentIndex(selected);
  table->setCellWidget(row, col, box);
  return box;
}
int ChoiceIndex(QTableWidget *t, int r, int c)
{
  if(t->property("delegateChoices").toBool())
    return t->item(r, c)->data(Qt::UserRole).toInt();
  return ((QComboBox *)t->cellWidget(r, c))->currentIndex();
}

// Camera tables can have thousands of choices. Instantiate only the cell being edited,
// instead of thousands of permanent combobox widgets and their layout/paint events.
class CameraChoiceDelegate : public QStyledItemDelegate
{
public:
  CameraChoiceDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                        const QModelIndex &index) const override
  {
    const QStringList values = index.data(Qt::UserRole + 1).toStringList();
    if(values.isEmpty())
      return QStyledItemDelegate::createEditor(parent, option, index);
    QComboBox *box = new QComboBox(parent);
    box->addItems(values);
    return box;
  }
  void setEditorData(QWidget *editor, const QModelIndex &index) const override
  {
    if(QComboBox *box = qobject_cast<QComboBox *>(editor))
      box->setCurrentIndex(index.data(Qt::UserRole).toInt());
    else
      QStyledItemDelegate::setEditorData(editor, index);
  }
  void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override
  {
    if(QComboBox *box = qobject_cast<QComboBox *>(editor))
    {
      model->setData(index, box->currentIndex(), Qt::UserRole);
      model->setData(index, box->currentText(), Qt::EditRole);
    }
    else
      QStyledItemDelegate::setModelData(editor, model, index);
  }
};
QString Cell(QTableWidget *t, int r, int c)
{
  return t->item(r, c) ? t->item(r, c)->text().trimmed() : QString();
}
void Cell(QTableWidget *t, int r, int c, QString value)
{
  t->setItem(r, c, new QTableWidgetItem(value));
}
QTableWidget *Table(QWidget *parent, const QStringList &headers)
{
  QTableWidget *t = new QTableWidget(0, headers.count(), parent);
  t->setHorizontalHeaderLabels(headers);
  t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  t->setSelectionBehavior(QAbstractItemView::SelectRows);
  return t;
}
QPushButton *Button(QHBoxLayout *layout, const QString &text, std::function<void()> fn)
{
  QPushButton *b = new QPushButton(text);
  layout->addWidget(b);
  QObject::connect(b, &QPushButton::clicked, b, fn);
  return b;
}
QString MatrixText(const QMatrix4x4 &m)
{
  QStringList rows;
  for(int r = 0; r < 4; r++)
  {
    QStringList cols;
    for(int c = 0; c < 4; c++)
      cols << QString::number(m(r, c), 'g', 9);
    rows << cols.join(lit(" "));
  }
  return rows.join(lit("\n"));
}
bool ParseMatrix(QString text, QMatrix4x4 &m)
{
  QStringList parts = text.split(QRegularExpression(lit("[\\s,;]+")), QString::SkipEmptyParts);
  if(parts.count() != 16)
    return false;
  for(int i = 0; i < 16; i++)
  {
    bool ok;
    float v = parts[i].toFloat(&ok);
    if(!ok || !std::isfinite(v))
      return false;
    m(i / 4, i % 4) = v;
  }
  bool invertible;
  m.inverted(&invertible);
  return invertible;
}
QMatrix4x4 DecodeMatrix(const bytebuf &bytes, bool rowMajor, bool rowVector)
{
  QMatrix4x4 m;
  float data[16];
  memcpy(data, bytes.data(), 64);
  for(int r = 0; r < 4; r++)
    for(int c = 0; c < 4; c++)
      m(r, c) = data[rowMajor ? 4 * r + c : 4 * c + r];
  return rowVector ? m.transposed() : m;
}
}

ReplayOverrides *ReplayOverrides::Find(ICaptureContext &ctx)
{
  return s_Editors.value(&ctx);
}
QString ReplayOverrides::PassLabel(ICaptureContext &ctx, uint32_t eid)
{
  ReplayOverrides *editor = Find(ctx);
  if(editor)
    for(const ReplayPassOverride &p : editor->m_Passes)
      if(eid >= p.firstEvent && eid <= p.lastEvent)
        return p.bypass ? lit("[Bypass] ") : lit("[Skipped] ");
  return {};
}

ReplayOverrides::ReplayOverrides(ICaptureContext &ctx, QWidget *parent, QWidget *render,
                                 std::function<void(const QJsonObject &)> display)
    : QObject(parent), m_Ctx(ctx), m_ApplyDisplay(display), m_Render(render)
{
  s_Editors[&ctx] = this;
  m_Toolbar = new QWidget(parent);
  QHBoxLayout *bar = new QHBoxLayout(m_Toolbar);
  bar->setContentsMargins(3, 0, 3, 0);
  Button(bar, tr("Free camera..."), [this]() { ShowCamera(); });
  m_Enable = Button(bar, tr("Enable"), [this]() {
    if(m_Enabled)
      ResetCamera();
    else
      EnableCamera();
  });
  Button(bar, tr("Reset view"), [this]() { ResetCamera(); });
  m_Speed = new QDoubleSpinBox(m_Toolbar);
  m_Speed->setRange(0.0001, 1000000);
  m_Speed->setDecimals(4);
  m_Speed->setValue(1);
  m_Speed->setPrefix(tr("Speed "));
  bar->addWidget(m_Speed);
  m_CameraState = new QLabel(tr("Original camera"), m_Toolbar);
  m_CameraState->setObjectName(lit("replayCameraState"));
  bar->addWidget(m_CameraState);
  m_SaveStatus = new QLabel(m_Toolbar);
  m_SaveStatus->setObjectName(lit("replayCameraSaveStatus"));
  bar->addWidget(m_SaveStatus);

  m_Dialog = new QDialog(parent);
  m_Dialog->setWindowTitle(tr("D3D11 replay overrides"));
  m_Dialog->resize(1100, 720);
  QVBoxLayout *layout = new QVBoxLayout(m_Dialog);
  QTabWidget *tabs = new QTabWidget(m_Dialog);
  tabs->setObjectName(lit("overrideTabs"));
  layout->addWidget(tabs);
  QWidget *camera = new QWidget(tabs);
  QVBoxLayout *cl = new QVBoxLayout(camera);
  QFormLayout *coordinates = new QFormLayout;
  m_WorldUp = new QComboBox(camera);
  m_WorldUp->setObjectName(lit("replayCameraWorldUp"));
  m_WorldUp->addItems(WorldUpAxes);
  m_WorldUp->setToolTip(tr("World-space up for yaw and Q/E movement. Apply to use this convention."));
  m_Forward = new QComboBox(camera);
  m_Forward->setObjectName(lit("replayCameraForward"));
  m_Forward->addItems({lit("-Z"), lit("+Z")});
  m_Forward->setToolTip(tr("Camera-local viewing direction. This does not change matrix storage or multiplication."));
  coordinates->addRow(tr("World up"), m_WorldUp);
  coordinates->addRow(tr("Camera forward"), m_Forward);
  cl->addLayout(coordinates);
  QLabel *help = new QLabel(
      tr("Map camera fields manually or from the current shader reflection. Offsets are relative "
         "to the bound CB range.\nMatrices below use mathematical rows and column vectors. Read "
         "them from mapped View/Projection fields, or paste them manually.\nWASD move, Q/E "
         "down/up, arrows or right-drag turn, Shift accelerates. Focus the texture to move."));
  help->setWordWrap(true);
  cl->addWidget(help);
  QHBoxLayout *automatic = new QHBoxLayout;
  cl->addLayout(automatic);
  m_AutoConfigure = Button(automatic, tr("Auto configure"), [this]() { StartCameraScan(); });
  m_AutoConfigure->setObjectName(lit("replayCameraAutoConfigure"));
  Button(automatic, tr("Recipe..."), [this]() { EditCameraRecipe(); })
      ->setObjectName(lit("replayCameraRecipe"));
  m_CancelScan = Button(automatic, tr("Cancel scan"), [this]() { CancelCameraScan(); });
  m_CancelScan->setObjectName(lit("replayCameraCancelScan"));
  m_CancelScan->setEnabled(false);
  m_ApplyPartial = Button(automatic, tr("Apply confirmed configuration"), [this]() { ApplyScanResult(); });
  m_ApplyPartial->setObjectName(lit("replayCameraApplyPartial"));
  m_ApplyPartial->setEnabled(false);
  m_ScanProgress = new QProgressBar(camera);
  automatic->addWidget(m_ScanProgress);
  m_ScanReport = new QPlainTextEdit(camera);
  m_ScanReport->setObjectName(lit("replayCameraScanReport"));
  m_ScanReport->setReadOnly(true);
  m_ScanReport->setMaximumHeight(110);
  m_ScanReport->setPlainText(
      tr("Import a configuration JSON, or ask an agent to analyse this capture and fill the recipe."));
  cl->addWidget(m_ScanReport);
  m_CameraTable = Table(
      camera, {tr("Value"), tr("First EID"), tr("Last EID"), tr("Shader ID"), tr("Stage"),
                 tr("CB slot"), tr("Byte offset"), tr("Storage"), tr("Multiply"), tr("Projection"), tr("Model")});
  cl->addWidget(m_CameraTable);
  m_CameraTable->setObjectName(lit("replayCameraFields"));
  m_CameraTable->setProperty("delegateChoices", true);
  m_CameraTable->setItemDelegate(new CameraChoiceDelegate(m_CameraTable));
  m_CameraTable->horizontalHeader()->setResizeContentsPrecision(32);
  QHBoxLayout *cb = new QHBoxLayout;
  cl->addLayout(cb);
  Button(cb, tr("Add field"), [this]() {
    CameraField f;
    f.patch.lastEvent = m_Ctx.GetLastAction() ? m_Ctx.GetLastAction()->eventId : 1;
    f.patch.shader = m_Ctx.CurPipelineState().GetShader(ShaderStage::Vertex);
    AddCameraField(f);
  });
  Button(cb, tr("From reflection..."), [this]() { AddReflectedField(); });
  Button(cb, tr("Remove row"), [this]() { m_CameraTable->removeRow(m_CameraTable->currentRow()); });
  Button(cb, tr("Read View / Projection"), [this]() { ReadBaseline(); });
  QHBoxLayout *matrices = new QHBoxLayout;
  cl->addLayout(matrices);
  m_View = new QPlainTextEdit(MatrixText(QMatrix4x4()), camera);
  m_Projection = new QPlainTextEdit(MatrixText(QMatrix4x4()), camera);
  m_View->setObjectName(lit("replayBaseView"));
  m_Projection->setObjectName(lit("replayBaseProjection"));
  for(auto pair :
      {qMakePair(tr("Base View"), m_View), qMakePair(tr("Base Projection"), m_Projection)})
  {
    QVBoxLayout *v = new QVBoxLayout;
    v->addWidget(new QLabel(pair.first));
    v->addWidget(pair.second);
    matrices->addLayout(v);
  }
  QHBoxLayout *apply = new QHBoxLayout;
  cl->addLayout(apply);
  Button(apply, tr("Apply and enable camera"), [this]() {
    EnableCamera();
  })->setObjectName(lit("replayCameraApplyEnable"));
  Button(apply, tr("Restore original camera"), [this]() { ResetCamera(); });
  m_ApplyProgress = new QProgressBar(camera);
  m_ApplyProgress->setObjectName(lit("replayCameraApplyProgress"));
  m_ApplyProgress->setRange(0, 0);
  m_ApplyProgress->hide();
  m_ApplyStatus = new QLabel(camera);
  m_ApplyStatus->setObjectName(lit("replayCameraApplyStatus"));
  cl->addWidget(m_ApplyStatus);
  cl->addWidget(m_ApplyProgress);
  for(QPushButton *button : camera->findChildren<QPushButton *>())
    m_ApplyControls.push_back(button);
  QWidget *controls[] = {m_Enable, m_CameraTable, m_View, m_Projection, m_Speed, m_Forward, m_WorldUp};
  for(QWidget *control : controls)
    m_ApplyControls.push_back(control);
  tabs->addTab(camera, tr("Camera"));

  QWidget *passes = new QWidget(tabs);
  QVBoxLayout *pl = new QVBoxLayout(passes);
  QLabel *ph = new QLabel(
      tr("Skip suppresses GPU actions but retains bindings and uploads. Bypass forwards the entry "
         "colour at range exit.\nUse multiple rows with the same range for multiple colour "
         "mappings. View formats are numeric DXGI_FORMAT values (0 = resource format).\nChoose "
         "linear Float/UNorm views. Temporal history and auxiliary outputs are not rebuilt."));
  ph->setWordWrap(true);
  pl->addWidget(ph);
  m_PassTable = Table(
      passes, {tr("First EID"), tr("Last EID"), tr("Mode"), tr("Input ID"), tr("Mip"), tr("Slice"),
               tr("View format"), tr("Output ID"), tr("Mip"), tr("Slice"), tr("View format")});
  pl->addWidget(m_PassTable);
  m_PassTable->setObjectName(lit("replayPassRules"));
  QHBoxLayout *pb = new QHBoxLayout;
  pl->addLayout(pb);
  Button(pb, tr("Add range"), [this]() { AddPassRow(m_Ctx.CurSelectedEvent(), m_Ctx.CurEvent()); });
  Button(pb, tr("Remove row"), [this]() { m_PassTable->removeRow(m_PassTable->currentRow()); });
  Button(pb, tr("Apply passes"), [this]() { ApplyPasses(); });
  Button(pb, tr("Restore all passes"), [this]() {
    m_PassTable->setRowCount(0);
    ApplyPasses();
  });
  tabs->addTab(passes, tr("Passes"));
  m_Status = new QLabel(m_Dialog);
  m_Status->setObjectName(lit("replayOverrideStatus"));
  m_Status->setWordWrap(true);
  layout->addWidget(m_Status);
  WatchInput(render);
  QObject::connect(&m_Timer, &QTimer::timeout, this, [this]() { Tick(); });
  m_Timer.setInterval(16);
  m_Timer.start();
  m_Clock.start();
  connect(m_CameraTable, &QTableWidget::itemChanged, this,
          [this](QTableWidgetItem *) { ++m_ConfigRevision; });
  connect(m_View, &QPlainTextEdit::textChanged, this, [this]() { ++m_ConfigRevision; });
  connect(m_Projection, &QPlainTextEdit::textChanged, this, [this]() { ++m_ConfigRevision; });
  m_Ctx.AddCaptureViewer(this);
  if(!m_Ctx.IsCaptureLoaded())
    OnCaptureClosed();
}

ReplayOverrides::~ReplayOverrides()
{
  CancelCameraScan();
  m_Timer.stop();
  m_Ctx.RemoveCaptureViewer(this);
  s_Editors.remove(&m_Ctx);
}
void ReplayOverrides::OnCaptureLoaded()
{
  OnEventChanged(0);
  // Viewers can be constructed/notified while the capture UI is still being enabled.
  const uint64_t epoch = m_Epoch;
  QTimer::singleShot(0, this, [this, epoch]() {
    if(epoch != m_Epoch || !m_Ctx.IsCaptureLoaded())
      return;
    OnEventChanged(0);
    if(m_Ctx.APIProps().pipelineType == GraphicsAPI::D3D11 &&
       !m_Ctx.Replay().CurrentRemote().IsValid())
      ReadCameraSidecar();
  });
}
void ReplayOverrides::OnEventChanged(uint32_t)
{
  m_Toolbar->setEnabled(m_Ctx.IsCaptureLoaded() &&
                        m_Ctx.APIProps().pipelineType == GraphicsAPI::D3D11 &&
                        !m_Ctx.Replay().CurrentRemote().IsValid());
}
void ReplayOverrides::OnCaptureClosed()
{
  ++m_Epoch;
  ++m_Submission;
  if(m_StagingApply)
  {
    m_StagingApply = false;
    SetApplyControls(false);
    m_ApplyProgress->hide();
  }
  if(m_Applying)
    FinishApply(tr("Capture closed."));
  m_ApplyPhase.clear();
  m_ApplyStatus->clear();
  m_SaveStatus->clear();
  m_LastUpdateMS = 0;
  m_ApplyTimings = QJsonObject();
  m_ResourceLookup.clear();
  m_ShaderNames.clear();
  m_ResourceLookupSize = 0;
  CancelCameraScan();
  m_Scan.reset();
  m_AutoConfigure->setEnabled(true);
  m_CancelScan->setEnabled(false);
  m_ScanResult = m_DisplayConfig = m_SaveAfterApply = m_PendingSave = QJsonObject();
  m_Recipe = QJsonObject();
  m_ConfigPasses = QJsonArray();
  m_ManagePasses = m_AppliedManagePasses = false;
  m_RestoreCamera = {};
  m_BeforeApplyForm = QJsonObject();
  m_WorldFrame.setToIdentity();
  m_WorldUp->setCurrentIndex(0);
  m_AutoConfigure->setText(tr("Auto configure"));
  m_AppliedPatches.clear();
  m_SidecarBusy = false;
  ++m_ConfigRevision;
  m_ApplyPartial->setEnabled(false);
  m_ScanReport->setPlainText(
      tr("Import a configuration JSON, or ask an agent to analyse this capture and fill the recipe."));
  m_ScanProgress->setValue(0);
  m_Enabled = m_Busy = m_Dirty = m_Dragging = false;
  m_Keys.clear();
  m_Fields.clear();
  m_Passes.clear();
  m_CameraTable->setRowCount(0);
  m_PassTable->setRowCount(0);
  m_Dialog->hide();
  m_Enable->setText(tr("Enable"));
  m_Toolbar->setEnabled(false);
  m_View->setPlainText(MatrixText(QMatrix4x4()));
  m_Projection->setPlainText(MatrixText(QMatrix4x4()));
  m_Status->clear();
  m_LastCameraError.clear();
  m_CameraState->setText(tr("Original camera"));
}
void ReplayOverrides::Error(const QString &text)
{
  m_Status->setText(text);
}
void ReplayOverrides::ShowCamera()
{
  // MCP can configure a window that was launched hidden. Make the actual viewer reachable too.
  m_Ctx.GetMainWindow()->Widget()->show();
  m_Dialog->findChild<QTabWidget *>(lit("overrideTabs"))->setCurrentIndex(0);
  m_Dialog->show();
  m_Dialog->raise();
}
void ReplayOverrides::ShowPass(uint32_t first, uint32_t last, bool add)
{
  if(add)
    AddPassRow(first, last);
  m_Dialog->findChild<QTabWidget *>(lit("overrideTabs"))->setCurrentIndex(1);
  m_Dialog->show();
  m_Dialog->raise();
}
ResourceId ReplayOverrides::LookupResource(const QString &text)
{
  const auto &resources = m_Ctx.GetResources();
  if(m_ResourceLookupSize != resources.size())
  {
    m_ResourceLookup.clear();
    m_ShaderNames.clear();
    for(const ResourceDescription &r : resources)
    {
      QString id = ToQStr(r.resourceId);
      m_ResourceLookup[id] = r.resourceId;
      m_ResourceLookup[id.section(lit("::"), -1)] = r.resourceId;
      if(r.type == ResourceType::Shader)
        m_ShaderNames.insert(id);
    }
    m_ResourceLookupSize = resources.size();
  }
  return m_ResourceLookup.value(text);
}
void ReplayOverrides::AddCameraField(const CameraField &f)
{
  int row = m_CameraTable->rowCount();
  m_CameraTable->insertRow(row);
  Choice(m_CameraTable, row, 0, Semantics, f.semantic);
  Cell(m_CameraTable, row, 1, QString::number(f.patch.firstEvent));
  Cell(m_CameraTable, row, 2, QString::number(f.patch.lastEvent));
  Cell(m_CameraTable, row, 3, ToQStr(f.patch.shader));
  Choice(m_CameraTable, row, 4, Stages, int(f.patch.stage));
  Cell(m_CameraTable, row, 5, QString::number(f.patch.slot));
  Cell(m_CameraTable, row, 6, QString::number(f.patch.byteOffset));
  Choice(m_CameraTable, row, 7, {lit("Row-major"), lit("Column-major")}, f.rowMajor ? 0 : 1);
  Choice(m_CameraTable, row, 8, {lit("M * v"), lit("v * M")}, f.rowVector ? 1 : 0);
  Cell(m_CameraTable, row, 9, f.hasProjection ? MatrixText(f.projection) : QString());
  m_CameraTable->item(row, 9)->setToolTip(
      tr("Blank uses Base Projection. Otherwise enter 16 mathematical row-ordered values."));
  Cell(m_CameraTable, row, 10, f.semantic == 9 ? MatrixText(f.model) : QString());
}
bool ReplayOverrides::ReadCameraFields(rdcarray<CameraField> &fields)
{
  fields.clear();
  for(int row = 0; row < m_CameraTable->rowCount(); row++)
  {
    CameraField f;
    f.semantic = ChoiceIndex(m_CameraTable, row, 0);
    f.patch.stage = ShaderStage(ChoiceIndex(m_CameraTable, row, 4));
    f.rowMajor = ChoiceIndex(m_CameraTable, row, 7) == 0;
    f.rowVector = ChoiceIndex(m_CameraTable, row, 8) == 1;
    if(f.semantic == 9 && !ParseMatrix(Cell(m_CameraTable, row, 10), f.model))
    {
      Error(tr("ModelView row %1 needs a finite, invertible Model matrix.").arg(row + 1));
      return false;
    }
    f.hasProjection = !Cell(m_CameraTable, row, 9).isEmpty();
    if(f.hasProjection && !ParseMatrix(Cell(m_CameraTable, row, 9), f.projection))
    {
      Error(tr("Invalid independent projection on row %1.").arg(row + 1));
      return false;
    }
    f.patch.shader = LookupResource(Cell(m_CameraTable, row, 3));
    bool ok = true;
    for(auto pair : {qMakePair(1, &f.patch.firstEvent), qMakePair(2, &f.patch.lastEvent),
                     qMakePair(5, &f.patch.slot), qMakePair(6, &f.patch.byteOffset)})
    {
      bool parsed;
      *pair.second = Cell(m_CameraTable, row, pair.first).toUInt(&parsed);
      ok &= parsed;
    }
    if(!ok || !f.patch.firstEvent || f.patch.firstEvent > f.patch.lastEvent ||
       f.patch.shader == ResourceId() || f.patch.slot >= 14)
    {
      Error(tr("Invalid camera row %1. Check event range, shader ID and CB slot.").arg(row + 1));
      return false;
    }
    fields.push_back(f);
  }
  if(fields.empty())
  {
    Error(tr("Add at least one camera field."));
    return false;
  }
  return true;
}
void ReplayOverrides::AddReflectedField()
{
  bool ok = false;
  QString selected = QInputDialog::getItem(m_Dialog, tr("Shader stage"), tr("Current event stage"),
                                           Stages, 0, false, &ok);
  if(!ok)
    return;
  ShaderStage stage = ShaderStage(Stages.indexOf(selected));
  const ShaderReflection *refl = m_Ctx.CurPipelineState().GetShaderReflection(stage);
  if(!refl)
  {
    Error(tr("No reflection at this stage. Add a field manually."));
    return;
  }
  rdcarray<CameraField> candidates;
  QStringList names;
  std::function<void(const rdcarray<ShaderConstant> &, uint32_t, uint32_t, QString)> visit;
  visit = [&](const rdcarray<ShaderConstant> &vars, uint32_t slot, uint32_t base, QString prefix) {
    for(const ShaderConstant &v : vars)
    {
      QString name = prefix + QString(v.name);
      uint32_t off = base + v.byteOffset;
      if(v.type.elements > 1)
        continue;
      if(!v.type.members.empty())
      {
        visit(v.type.members, slot, off, name + lit("."));
        continue;
      }
      if(v.type.baseType != VarType::Float ||
         !((v.type.rows == 4 && v.type.columns == 4) || (v.type.rows == 1 && v.type.columns >= 3)))
        continue;
      CameraField f;
      f.patch.stage = stage;
      f.patch.shader = m_Ctx.CurPipelineState().GetShader(stage);
      f.patch.slot = slot;
      f.patch.byteOffset = off;
      f.patch.lastEvent = m_Ctx.GetLastAction()->eventId;
      f.rowMajor = v.type.RowMajor();
      f.semantic = v.type.rows == 1 ? 6 : 0;
      candidates.push_back(f);
      names << QString(lit("b%1 +%2: %3")).arg(slot).arg(off).arg(name);
    }
  };
  for(const ConstantBlock &block : refl->constantBlocks)
    visit(block.variables, block.fixedBindNumber, 0, QString(block.name) + lit("."));
  if(names.empty())
  {
    Error(tr("No supported float4x4 or position fields. Add a field manually."));
    return;
  }
  selected = QInputDialog::getItem(
      m_Dialog, tr("Camera field"),
      tr("Choose a field, then assign its camera meaning in the table."), names, 0, false, &ok);
  if(ok)
    AddCameraField(candidates[names.indexOf(selected)]);
}
void ReplayOverrides::ReadBaseline()
{
  rdcarray<CameraField> fields;
  if(!ReadCameraFields(fields))
    return;
  QMatrix4x4 view, proj;
  bool haveView = false, haveProj = false;
  m_Ctx.Replay().BlockInvoke([&](IReplayController *r) {
    for(const CameraField &f : fields)
    {
      if(f.semantic != 0 && f.semantic != 1)
        continue;
      const PipeState &pipe = r->GetPipelineState();
      const ShaderReflection *ref = pipe.GetShaderReflection(f.patch.stage);
      if(!ref || pipe.GetShader(f.patch.stage) != f.patch.shader)
        continue;
      for(uint32_t b = 0; b < ref->constantBlocks.size(); b++)
      {
        if(ref->constantBlocks[b].fixedBindNumber != f.patch.slot)
          continue;
        Descriptor d = pipe.GetConstantBlock(f.patch.stage, b, 0).descriptor;
        if(uint64_t(f.patch.byteOffset) + 64 > d.byteSize)
          continue;
        bytebuf bytes = r->GetBufferData(d.resource, d.byteOffset + f.patch.byteOffset, 64);
        if(bytes.size() != 64)
          continue;
        QMatrix4x4 matrix = DecodeMatrix(bytes, f.rowMajor, f.rowVector);
        if(f.semantic == 0 && !haveView)
        {
          view = matrix;
          haveView = true;
        }
        if(f.semantic == 1 && !haveProj)
        {
          proj = matrix;
          haveProj = true;
        }
      }
    }
  });
  if(haveView)
    m_View->setPlainText(MatrixText(view));
  if(haveProj)
    m_Projection->setPlainText(MatrixText(proj));
  Error(haveView && haveProj ? tr("Read base View and Projection from the selected event.")
                             : tr("Could not read both matrices. Select an event with the mapped "
                                  "bindings, or paste the missing matrix manually."));
}
void ReplayOverrides::EnableCamera()
{
  if(m_Scan || m_Applying || m_StagingApply || m_Busy || m_Dirty)
    return;
  if(m_BeforeApplyForm.isEmpty())
    m_BeforeApplyForm = QJsonDocument::fromJson(QString(CameraState()).toUtf8()).object();
  const auto fields = m_Fields;
  const auto pose = m_Pose, projection = m_Proj, worldFrame = m_WorldFrame;
  const auto position = m_Position;
  const float yaw = m_Yaw, pitch = m_Pitch, handedness = m_Handedness, forward = m_ForwardDirection;
  const bool enabled = m_Enabled;
  m_RestoreCamera = [this, fields, pose, projection, worldFrame, position, yaw, pitch, handedness, forward, enabled]() {
    m_Fields = fields; m_Pose = pose; m_Proj = projection; m_Position = position;
    m_WorldFrame = worldFrame;
    m_Yaw = yaw; m_Pitch = pitch; m_Handedness = handedness; m_Enabled = enabled;
    m_ForwardDirection = forward;
    m_Enable->setText(enabled ? tr("Disable") : tr("Enable"));
  };
  m_Applying = true;
  m_ApplyClock.start();
  SetApplyControls(true);
  m_Keys.clear();
  m_Dragging = false;
  SetApplyPhase(lit("preparing"));
  const uint64_t epoch = m_Epoch;
  QTimer::singleShot(0, this, [this, epoch]() {
    if(epoch == m_Epoch && m_Applying)
      PrepareCamera();
  });
}

void ReplayOverrides::SetApplyControls(bool disabled)
{
  if(!disabled)
  {
    for(auto it = m_ApplyControlStates.begin(); it != m_ApplyControlStates.end(); ++it)
      it.key()->setEnabled(it.value());
    m_ApplyControlStates.clear();
    return;
  }
  m_ApplyControlStates.clear();
  for(QWidget *widget : m_ApplyControls)
  {
    m_ApplyControlStates[widget] = widget->isEnabled();
    widget->setEnabled(false);
  }
}

void ReplayOverrides::SetApplyPhase(const QString &phase)
{
  m_ApplyPhase = phase;
  m_ApplyProgress->setVisible(m_Applying || m_StagingApply);
  const QString text = phase == lit("preparing")    ? tr("Preparing camera data...")
                       : phase == lit("replaying")  ? tr("Replaying scene...")
                       : phase == lit("refreshing") ? tr("Refreshing image...")
                                                    : QString();
  m_ApplyStatus->setText(text);
  if(!text.isEmpty())
    m_CameraState->setText(text);
}

void ReplayOverrides::FinishApply(const QString &error)
{
  m_Applying = false;
  m_ApplyProgress->hide();
  SetApplyControls(false);
  m_ApplyPhase = error.isEmpty() ? lit("complete") : lit("failed");
  m_ApplyStatus->setText(error.isEmpty() ? tr("Camera ready.") : error);
  if(!error.isEmpty())
  {
    m_LastCameraError = error;
    Error(error);
  }
  else
  {
    m_ApplyPartial->setEnabled(false);
    m_Dialog->hide();
    m_Ctx.RaiseDockWindow(m_Ctx.GetTextureViewer()->Widget());
    m_Render->setFocus(Qt::OtherFocusReason);
  }
  m_CameraState->setText(m_Enabled ? tr("Camera active") : tr("Original camera"));
}

void ReplayOverrides::PrepareCamera()
{
  QElapsedTimer preparation;
  preparation.start();
  m_LastCameraError.clear();
  if(!m_Ctx.IsCaptureLoaded() || m_Ctx.APIProps().pipelineType != GraphicsAPI::D3D11)
  {
    FinishApply(tr("Free camera requires a loaded D3D11 capture."));
    return;
  }
  rdcarray<CameraField> fields;
  if(!ReadCameraFields(fields))
  {
    FinishApply(m_Status->text());
    return;
  }
  QMatrix4x4 view, proj;
  if(!ParseMatrix(m_View->toPlainText(), view) || !ParseMatrix(m_Projection->toPlainText(), proj))
  {
    FinishApply(tr("View and Projection each need 16 finite values and must be invertible."));
    return;
  }
  m_Fields = fields;
  m_TargetEvent = uint32_t(m_DisplayConfig.value(lit("event_id")).toDouble(m_Ctx.CurEvent()));
  m_Proj = proj;
  m_ForwardDirection = m_Forward->currentIndex() == 0 ? -1.0f : 1.0f;
  m_Pose = view.inverted();
  m_Position = m_Pose.column(3).toVector3D();
  const QVector3D right = m_Pose.column(0).toVector3D();
  const QVector3D up = m_Pose.column(1).toVector3D();
  const QVector3D z = m_Pose.column(2).toVector3D().normalized();
  m_Handedness = QVector3D::dotProduct(QVector3D::crossProduct(right, up), z) < 0 ? -1.0f : 1.0f;
  QVector3D worldUp;
  const QString axis = m_WorldUp->currentText();
  worldUp[axis[1] == QLatin1Char('X') ? 0 : axis[1] == QLatin1Char('Y') ? 1 : 2] = axis[0] == QLatin1Char('-') ? -1.0f : 1.0f;
  // Choose a fixed horizontal zero-heading. Only yaw's numeric origin depends on it.
  const QVector3D heading = qAbs(worldUp.z()) > .5f ? QVector3D(1, 0, 0) : QVector3D(0, 0, 1);
  m_WorldFrame.setToIdentity();
  m_WorldFrame.setColumn(0, QVector4D(QVector3D::crossProduct(worldUp, heading), 0));
  m_WorldFrame.setColumn(1, QVector4D(worldUp, 0));
  m_WorldFrame.setColumn(2, QVector4D(heading, 0));
  const QVector3D localZ = m_WorldFrame.transposed().mapVector(z);
  const QVector3D localRight = m_WorldFrame.transposed().mapVector(right);
  constexpr float degrees = 180.0f / 3.14159265358979323846f;
  // These are absolute world angles, not deltas on a possibly pitched/rolled capture pose.
  // At a pole, use the captured right axis to choose a stable horizontal heading.
  m_Yaw = (localZ.x() * localZ.x() + localZ.z() * localZ.z() > 1e-8f
               ? std::atan2(localZ.x(), localZ.z())
               : std::atan2(-m_Handedness * localRight.z(), m_Handedness * localRight.x())) *
          degrees;
  m_Pitch = -std::asin(qBound(-1.0f, localZ.y(), 1.0f)) * degrees;
  UpdatePose();
  m_Enabled = true;
  m_Dirty = true;
  m_Enable->setText(tr("Disable"));
  m_CameraState->setText(tr("Updating..."));
  m_Keys.clear();
  ++m_ConfigRevision;
  m_SaveAfterApply = QJsonDocument::fromJson(QString(CameraState()).toUtf8()).object();
  m_ApplyTimings[lit("prepare_ms")] = double(preparation.nsecsElapsed()) / 1000000.0;
  SubmitCamera();
}
void ReplayOverrides::ResetCamera()
{
  if(m_Scan || m_Applying || m_StagingApply)
    return;
  m_SaveAfterApply = QJsonObject();
  m_LastCameraError.clear();
  m_Enabled = false;
  m_Dirty = true;
  m_Keys.clear();
  m_Dragging = false;
  m_Yaw = m_Pitch = 0;
  m_Enable->setText(tr("Enable"));
  m_CameraState->setText(tr("Updating..."));
  SubmitCamera();
}
void ReplayOverrides::UpdatePose()
{
  constexpr float radians = 3.14159265358979323846f / 180.0f;
  m_Yaw = std::remainder(m_Yaw, 360.0f);
  m_Pitch = qBound(-89.0f, m_Pitch, 89.0f);
  const float sy = std::sin(m_Yaw * radians), cy = std::cos(m_Yaw * radians);
  const float sp = std::sin(m_Pitch * radians), cp = std::cos(m_Pitch * radians);
  // The configured world up stays fixed. Pitch cannot introduce roll.
  // Keep mirrored view conventions (e.g. Unity's camera basis) without flipping geometry.
  m_Pose.setToIdentity();
  m_Pose.setColumn(0, QVector4D(m_Handedness * cy, 0, -m_Handedness * sy, 0));
  m_Pose.setColumn(1, QVector4D(sy * sp, cp, cy * sp, 0));
  m_Pose.setColumn(2, QVector4D(sy * cp, -sp, cy * cp, 0));
  m_Pose = m_WorldFrame * m_Pose;
  m_Pose.setColumn(3, QVector4D(m_Position, 1));
  m_Dirty = true;
}
float ReplayOverrides::HorizontalInputSign() const
{
  // Camera +X need not point right on screen: some games mirror X in Projection.
  // Use the projected X derivative, including homogeneous W, so multiplying the
  // entire projection by -1 does not reverse otherwise identical controls.
  const QVector4D clip = m_Proj * QVector4D(0, 0, m_ForwardDirection, 1);
  const float derivative = m_Proj(0, 0) * clip.w() - clip.x() * m_Proj(3, 0);
  return qAbs(clip.w()) > 1e-8f && derivative < 0 ? -1.0f : 1.0f;
}

void ReplayOverrides::Tick()
{
  float dt = float(qMin<qint64>(m_Clock.restart(), 100)) / 1000.0f;
  if(!m_Applying && !m_StagingApply && !m_Scan && m_Enabled && m_Render->hasFocus() && !m_Keys.empty())
  {
    float speed = float(m_Speed->value()) * dt * (m_Keys.contains(Qt::Key_Shift) ? 5.0f : 1.0f);
    float z = (m_Keys.contains(Qt::Key_W) - m_Keys.contains(Qt::Key_S)) *
              m_ForwardDirection;
    float x = (m_Keys.contains(Qt::Key_D) - m_Keys.contains(Qt::Key_A)) * HorizontalInputSign();
    if(x == 0 && z == 0 && m_Keys.contains(Qt::Key_Q) == m_Keys.contains(Qt::Key_E) &&
       m_Keys.contains(Qt::Key_Left) == m_Keys.contains(Qt::Key_Right) &&
       m_Keys.contains(Qt::Key_Up) == m_Keys.contains(Qt::Key_Down))
    {
      SubmitCamera();
      return;
    }
    m_Position +=
        m_Pose.column(0).toVector3D() * (x * speed) + m_Pose.column(2).toVector3D() * (z * speed);
    m_Position += m_WorldFrame.column(1).toVector3D() *
                  ((m_Keys.contains(Qt::Key_E) - m_Keys.contains(Qt::Key_Q)) * speed);
    const float lookSign = -m_ForwardDirection;
    m_Yaw += HorizontalInputSign() * m_Handedness * lookSign *
             (m_Keys.contains(Qt::Key_Left) - m_Keys.contains(Qt::Key_Right)) * 60.0f * dt;
    m_Pitch =
        qBound(-89.0f,
               m_Pitch + lookSign * (m_Keys.contains(Qt::Key_Up) - m_Keys.contains(Qt::Key_Down)) *
                             60.0f * dt,
               89.0f);
    UpdatePose();
  }
  SubmitCamera();
}
void ReplayOverrides::WatchInput(QWidget *widget)
{
  widget->installEventFilter(this);
  for(QWidget *child : widget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
    WatchInput(child);
}

bool ReplayOverrides::eventFilter(QObject *watched, QEvent *event)
{
  QWidget *widget = qobject_cast<QWidget *>(watched);
  if(!widget || (widget != m_Render && !m_Render->isAncestorOf(widget)))
    return false;
  // CustomPaintWidget recreates the native child on capture/output changes. Mouse signals
  // emitted by that child bypass filters on the outer widget, so watch the real receiver too.
  if(event->type() == QEvent::ChildAdded || event->type() == QEvent::ChildPolished)
  {
    if(QWidget *child = qobject_cast<QWidget *>(static_cast<QChildEvent *>(event)->child()))
      WatchInput(child);
    return false;
  }
  if(event->type() == QEvent::ChildRemoved || event->type() == QEvent::UngrabMouse)
    m_Dragging = false;
  if(event->type() == QEvent::FocusOut || event->type() == QEvent::Hide)
  {
    m_Keys.clear();
    m_Dragging = false;
  }
  if(!m_Enabled || m_Scan || m_Applying || m_StagingApply)
    return false;
  if(event->type() == QEvent::ContextMenu)
    return true;
  if(event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ||
     event->type() == QEvent::KeyRelease)
  {
    QKeyEvent *key = static_cast<QKeyEvent *>(event);
    if(event->type() == QEvent::KeyRelease && !key->isAutoRepeat() && m_Keys.contains(key->key()))
    {
      m_Keys.remove(key->key());
      event->accept();
      return true;
    }
    if(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
      return false;
    int k = key->key();
    if(k == Qt::Key_W || k == Qt::Key_A || k == Qt::Key_S || k == Qt::Key_D || k == Qt::Key_Q ||
       k == Qt::Key_E || k == Qt::Key_Shift || k == Qt::Key_Left || k == Qt::Key_Right ||
       k == Qt::Key_Up || k == Qt::Key_Down)
    {
      if(event->type() == QEvent::ShortcutOverride)
      {
        event->accept();
        return true;
      }
      if(!key->isAutoRepeat())
      {
        if(event->type() == QEvent::KeyPress)
          m_Keys.insert(k);
        else
          m_Keys.remove(k);
      }
      event->accept();
      return true;
    }
  }
  if(event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
     event->type() == QEvent::MouseButtonDblClick)
  {
    QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
    if(mouse->button() == Qt::RightButton)
    {
      if(event->type() == QEvent::MouseButtonRelease)
        m_Dragging = false;
      else
      {
        m_Render->setFocus(Qt::MouseFocusReason);
        m_Mouse = widget->mapTo(m_Render, mouse->pos());
        m_Dragging = true;
      }
      event->accept();
      return true;
    }
  }
  if(event->type() == QEvent::MouseMove)
  {
    QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
    if(!(mouse->buttons() & Qt::RightButton))
    {
      m_Dragging = false;
      return false;
    }
    // A press outside this view, or a cancelled grab, must not resume rotation or pick pixels.
    event->accept();
    if(!m_Dragging)
      return true;
    QPoint pos = widget->mapTo(m_Render, mouse->pos()), delta = pos - m_Mouse;
    m_Mouse = pos;
    const float lookSign = -m_ForwardDirection;
    m_Yaw -= HorizontalInputSign() * m_Handedness * lookSign * delta.x() * 0.2f;
    m_Pitch = qBound(-89.0f, m_Pitch - lookSign * delta.y() * 0.2f, 89.0f);
    UpdatePose();
    return true;
  }
  return false;
}
void ReplayOverrides::SubmitCamera()
{
  if(!m_Dirty || m_Busy || m_Scan || m_StagingApply || !m_Ctx.IsCaptureLoaded())
    return;
  const auto fields = m_Fields;
  const QMatrix4x4 pose = m_Pose, projection = m_Proj;
  const QVector3D position = m_Position;
  const bool enabled = m_Enabled, applying = m_Applying;
  const bool combined = applying || (!enabled && m_AppliedManagePasses);
  const bool managed = applying ? m_ManagePasses : m_AppliedManagePasses;
  rdcarray<ReplayPassOverride> passes = m_Passes;
  if(applying && managed)
  {
    rdcstr error = DecodePasses(m_ConfigPasses, passes);
    if(!error.empty()) { if(m_RestoreCamera) m_RestoreCamera(); m_Dirty = false; FinishApply(QString(error)); return; }
  }
  if(!enabled && managed) passes.clear();
  const uint32_t target = applying ? m_TargetEvent : m_Ctx.CurEvent();
  const uint64_t epoch = m_Epoch, submission = ++m_Submission;
  const QJsonObject save = m_SaveAfterApply, display = m_DisplayConfig;
  m_SaveAfterApply = QJsonObject();
  m_Busy = true;
  m_Dirty = false;
  if(!applying)
    m_CameraState->setText(tr("Updating..."));
  QPointer<ReplayOverrides> weak(this);
  m_Ctx.Replay().AsyncInvoke([weak, fields, pose, projection, position, enabled, applying, target,
                              epoch, submission, save, display, combined, managed, passes](IReplayController *r) {
    QElapsedTimer timer;
    timer.start();
    rdcarray<ReplayConstantOverride> patches;
    if(enabled)
    {
      const QMatrix4x4 view = pose.inverted();
      QMatrix4x4 relativeView = view;
      relativeView(0, 3) = relativeView(1, 3) = relativeView(2, 3) = 0;
      QMap<QByteArray, QMap<int, QMatrix4x4>> cache;
      for(const CameraField &f : fields)
      {
        ReplayConstantOverride patch = f.patch;
        float data[16] = {};
        if(f.semantic == 6)
        {
          data[0] = position.x();
          data[1] = position.y();
          data[2] = position.z();
          patch.data.resize(12);
        }
        else
        {
          const QMatrix4x4 proj = f.hasProjection ? f.projection : projection;
          // Bitwise grouping preserves even small differences in projection jitter.
          auto &matrices = cache[QByteArray((const char *)proj.constData(), 16 * sizeof(float))];
          if(!matrices.contains(f.semantic))
          {
            if((f.semantic == 2 || f.semantic == 5) && !matrices.contains(2))
              matrices[2] = proj * view;
            if((f.semantic == 7 || f.semantic == 8) && !matrices.contains(7))
              matrices[7] = proj * relativeView;
            switch(f.semantic)
            {
              case 0: matrices[0] = view; break;
              case 1: matrices[1] = proj; break;
              case 3: matrices[3] = pose; break;
              case 4: matrices[4] = proj.inverted(); break;
              case 5: matrices[5] = matrices[2].inverted(); break;
              case 8: matrices[8] = matrices[7].inverted(); break;
            }
          }
          QMatrix4x4 matrix = f.semantic == 9 ? view * f.model : matrices[f.semantic];
          if(f.rowVector)
            matrix = matrix.transposed();
          for(int row = 0; row < 4; row++)
            for(int col = 0; col < 4; col++)
              data[f.rowMajor ? row * 4 + col : col * 4 + row] = matrix(row, col);
          patch.data.resize(64);
        }
        memcpy(patch.data.data(), data, patch.data.size());
        patches.push_back(patch);
      }
    }
    if(applying && weak)
      GUIInvoke::call(weak, [weak, epoch, submission]() {
        if(weak && weak->m_Epoch == epoch && weak->m_Submission == submission)
          weak->SetApplyPhase(lit("replaying"));
      });
    rdcstr error = combined ? r->SetReplayOverrides(patches, passes, target)
                           : r->SetReplayConstantOverrides(patches, target);
    const double elapsed = double(timer.nsecsElapsed()) / 1000000.0;
    const D3D11Pipe::State *d3d11 = r->GetD3D11PipelineState();
    const PipeState *pipeline = &r->GetPipelineState();
    if(weak)
      GUIInvoke::call(weak, [weak, patches, error, elapsed, applying, epoch, submission, target,
                             save, display, d3d11, pipeline, combined, managed, passes]() {
        if(!weak || weak->m_Epoch != epoch || weak->m_Submission != submission)
          return;
        weak->m_LastUpdateMS = elapsed;
        if(applying)
          weak->m_ApplyTimings[lit("worker_ms")] = elapsed;
        if(!error.empty())
        {
          weak->m_Busy = false;
          weak->m_LastCameraError = QString(error);
          weak->Error(QString(error));
          if(applying)
          {
            if(weak->m_RestoreCamera) weak->m_RestoreCamera();
            weak->m_RestoreCamera = {};
            weak->m_Dirty = false;
            weak->FinishApply(QString(error));
            // Restore the visible configuration as well as the replay transaction.
            const bool previousEnabled = weak->m_Enabled;
            QJsonObject previous = weak->m_BeforeApplyForm;
            previous[lit("enabled")] = false;
            previous[lit("show")] = true;
            if(previous[lit("event_id")].toInt() == 0) previous.remove(lit("event_id"));
            weak->m_Enabled = false;
            weak->m_RestoringForm = true;
            const rdcstr formError = weak->ConfigureCamera(QString::fromUtf8(QJsonDocument(previous).toJson()));
            if(!formError.empty())
            {
              weak->m_LastCameraError += tr("; restoring the form failed: %1").arg(QString(formError));
              weak->Error(weak->m_LastCameraError);
            }
            weak->m_RestoringForm = false;
            weak->m_Enabled = previousEnabled;
            weak->m_Enable->setText(previousEnabled ? tr("Disable") : tr("Enable"));
            if(!weak->m_ManagePasses) weak->PopulatePasses(weak->m_Passes);
            weak->m_BeforeApplyForm = QJsonObject();
          }
          else
          {
            weak->m_Enabled = false;
            weak->m_Enable->setText(tr("Enable"));
            weak->m_Dirty = !patches.empty();
            weak->SubmitCamera();
          }
          return;
        }
        weak->m_AppliedPatches = patches;
        if(combined)
        {
          weak->m_Passes = passes;
          if(applying) weak->m_AppliedManagePasses = managed;
          weak->RefreshViews();
        }
        weak->m_RestoreCamera = {};
        weak->m_BeforeApplyForm = QJsonObject();
        if(applying)
        {
          weak->SetApplyPhase(lit("refreshing"));
          QElapsedTimer publish;
          publish.start();
          static_cast<CaptureContext &>(weak->m_Ctx).CameraReplayEventReady(target, d3d11, pipeline);
          weak->m_ApplyDisplay(display);
          weak->m_ApplyTimings[lit("publish_ms")] = double(publish.nsecsElapsed()) / 1000000.0;
          weak->m_Render->update();
          // Display changes enqueue their replay work. Complete only after that work has run.
          weak->m_Ctx.Replay().AsyncInvoke([weak, epoch, submission, save](IReplayController *r) {
            const ResultDetails status = r->GetFatalErrorStatus();
            if(weak)
              GUIInvoke::call(weak, [weak, epoch, submission, save, status]() {
                if(!weak || weak->m_Epoch != epoch || weak->m_Submission != submission)
                  return;
                weak->m_Busy = false;
                weak->m_LastUpdateMS = double(weak->m_ApplyClock.nsecsElapsed()) / 1000000.0;
                weak->FinishApply(status.OK() ? QString() : QString(status.Message()));
                if(status.OK() && !save.isEmpty())
                  weak->SaveCamera(save);
              });
          });
        }
        else
        {
          weak->m_Busy = false;
          weak->m_CameraState->setText(weak->m_Dirty     ? tr("Updating...")
                                       : weak->m_Enabled ? tr("Camera active")
                                                         : tr("Original camera"));
          weak->m_Render->update();
          weak->SubmitCamera();
        }
      });
  });
}
void ReplayOverrides::AddPassRow(uint32_t first, uint32_t last)
{
  int r = m_PassTable->rowCount();
  m_PassTable->insertRow(r);
  Cell(m_PassTable, r, 0, QString::number(qMax(1U, first)));
  Cell(m_PassTable, r, 1, QString::number(qMax(first, last)));
  Choice(m_PassTable, r, 2, {tr("Normal"), tr("Skip"), tr("Colour bypass")}, 1);
  for(int c : {3, 7})
  {
    QComboBox *box = new QComboBox(m_PassTable);
    for(const TextureDescription &t : m_Ctx.GetTextures())
      if(t.dimension == 2 && t.msSamp == 1)
        box->addItem(ToQStr(t.resourceId) + lit(" ") + QString(m_Ctx.GetResourceName(t.resourceId)),
                     QVariant::fromValue(t.resourceId));
    int idx = box->findData(QVariant::fromValue(m_Ctx.GetTextureViewer()->GetCurrentResource()));
    if(idx >= 0)
      box->setCurrentIndex(idx);
    m_PassTable->setCellWidget(r, c, box);
  }
  for(int c : {4, 5, 6, 8, 9, 10})
    Cell(m_PassTable, r, c, lit("0"));
}
rdcstr ReplayOverrides::DecodePasses(const QJsonValue &value, rdcarray<ReplayPassOverride> &passes)
{
  if(!value.isArray() || value.toArray().size() > 1024)
    return "passes must be an array with at most 1024 entries.";
  auto readUInt = [](const QJsonObject &obj, const QString &key, uint32_t &result, bool optional) {
    if(optional && !obj.contains(key)) { result = 0; return true; }
    double n = obj[key].toDouble(-1);
    if(!obj[key].isDouble() || !std::isfinite(n) || n < 0 || n > double(UINT32_MAX) || n != std::floor(n)) return false;
    result = uint32_t(n); return true;
  };
  passes.clear();
  for(const QJsonValue &entry : value.toArray())
  {
    if(!entry.isObject()) return "Pass entries must be objects.";
    const QJsonObject obj = entry.toObject();
    ReplayPassOverride pass;
    const QString mode = obj[lit("mode")].toString();
    if(mode != lit("skip") && mode != lit("colour_bypass")) return "Invalid pass mode.";
    pass.bypass = mode == lit("colour_bypass");
    if(!readUInt(obj, lit("first_event"), pass.firstEvent, false) ||
       !readUInt(obj, lit("last_event"), pass.lastEvent, false) || !pass.firstEvent ||
       pass.firstEvent > pass.lastEvent || !m_Ctx.GetAction(pass.firstEvent) || !m_Ctx.GetAction(pass.lastEvent))
      return "Pass range must identify existing capture actions.";
    if(!obj[lit("reason")].isString() || obj[lit("reason")].toString().trimmed().isEmpty())
      return "Pass rules need an analysis reason.";
    if(obj.contains(lit("colours")) && !obj[lit("colours")].isArray()) return "colours must be an array.";
    const auto colours = obj[lit("colours")].toArray();
    if(pass.bypass == colours.isEmpty()) return "Colour mappings are required only for colour_bypass.";
    for(const QJsonValue &c : colours)
    {
      if(!c.isObject()) return "Colour mappings must be objects.";
      const auto mapping = c.toObject();
      ReplayColourMapping colour;
      for(const TextureDescription &texture : m_Ctx.GetTextures())
      {
        if(ToQStr(texture.resourceId) == mapping[lit("source")].toString()) colour.source = texture.resourceId;
        if(ToQStr(texture.resourceId) == mapping[lit("destination")].toString()) colour.destination = texture.resourceId;
      }
      if(colour.source == ResourceId() || colour.destination == ResourceId()) return "Colour mapping texture does not exist.";
      if(!readUInt(mapping, lit("source_mip"), colour.sourceMip, true) ||
         !readUInt(mapping, lit("source_slice"), colour.sourceSlice, true) ||
         !readUInt(mapping, lit("source_format"), colour.sourceFormat, true) ||
         !readUInt(mapping, lit("destination_mip"), colour.destinationMip, true) ||
         !readUInt(mapping, lit("destination_slice"), colour.destinationSlice, true) ||
         !readUInt(mapping, lit("destination_format"), colour.destinationFormat, true)) return "Invalid colour subresource or view format.";
      pass.colours.push_back(colour);
    }
    for(const auto &other : passes)
      if((pass.bypass || other.bypass) && pass.firstEvent <= other.lastEvent && other.firstEvent <= pass.lastEvent)
        return "Colour bypass ranges cannot overlap another pass override.";
    passes.push_back(pass);
  }
  return {};
}

void ReplayOverrides::PopulatePasses(const rdcarray<ReplayPassOverride> &passes)
{
  m_PassTable->setRowCount(0);
  for(const auto &pass : passes)
    for(size_t i = 0; i < qMax(size_t(1), pass.colours.size()); i++)
    {
      AddPassRow(pass.firstEvent, pass.lastEvent);
      const int row = m_PassTable->rowCount() - 1;
      ((QComboBox *)m_PassTable->cellWidget(row, 2))->setCurrentIndex(pass.bypass ? 2 : 1);
      if(!pass.bypass) continue;
      const auto &c = pass.colours[i];
      for(auto pair : {qMakePair(3, c.source), qMakePair(7, c.destination)})
      {
        auto box = (QComboBox *)m_PassTable->cellWidget(row, pair.first);
        box->setCurrentIndex(box->findData(QVariant::fromValue(pair.second)));
      }
      Cell(m_PassTable, row, 4, QString::number(c.sourceMip));
      Cell(m_PassTable, row, 5, QString::number(c.sourceSlice));
      Cell(m_PassTable, row, 6, QString::number(c.sourceFormat));
      Cell(m_PassTable, row, 8, QString::number(c.destinationMip));
      Cell(m_PassTable, row, 9, QString::number(c.destinationSlice));
      Cell(m_PassTable, row, 10, QString::number(c.destinationFormat));
    }
}

void ReplayOverrides::ApplyPasses()
{
  if(!m_Ctx.IsCaptureLoaded() || m_Scan || m_Busy || m_Dirty || m_Applying || m_StagingApply)
    return;
  rdcarray<ReplayPassOverride> passes;
  for(int r = 0; r < m_PassTable->rowCount(); r++)
  {
    int mode = ChoiceIndex(m_PassTable, r, 2);
    if(mode == 0)
      continue;
    ReplayPassOverride p;
    bool ok1, ok2;
    p.firstEvent = Cell(m_PassTable, r, 0).toUInt(&ok1);
    p.lastEvent = Cell(m_PassTable, r, 1).toUInt(&ok2);
    p.bypass = mode == 2;
    if(!ok1 || !ok2 || !p.firstEvent || p.firstEvent > p.lastEvent)
    {
      Error(tr("Invalid event range on row %1.").arg(r + 1));
      return;
    }
    if(p.bypass)
    {
      ReplayColourMapping c;
      c.source = ((QComboBox *)m_PassTable->cellWidget(r, 3))->currentData().value<ResourceId>();
      c.destination = ((QComboBox *)m_PassTable->cellWidget(r, 7))->currentData().value<ResourceId>();
      for(auto pair : {qMakePair(4, &c.sourceMip), qMakePair(5, &c.sourceSlice),
                       qMakePair(6, &c.sourceFormat), qMakePair(8, &c.destinationMip),
                       qMakePair(9, &c.destinationSlice), qMakePair(10, &c.destinationFormat)})
      {
        bool ok;
        *pair.second = Cell(m_PassTable, r, pair.first).toUInt(&ok);
        if(!ok)
        {
          Error(tr("Invalid mip, slice or format on row %1.").arg(r + 1));
          return;
        }
      }
      p.colours.push_back(c);
    }
    bool merged = false;
    for(ReplayPassOverride &old : passes)
      if(old.firstEvent == p.firstEvent && old.lastEvent == p.lastEvent && old.bypass == p.bypass)
      {
        for(const ReplayColourMapping &c : p.colours)
          old.colours.push_back(c);
        merged = true;
        break;
      }
    if(!merged)
      passes.push_back(p);
  }
  rdcstr error;
  m_Ctx.Replay().BlockInvoke([&](IReplayController *r) { error = r->SetReplayPassOverrides(passes); });
  if(!error.empty())
  {
    Error(QString(error));
    return;
  }
  m_Passes = passes;
  Error(tr("Applied %1 pass rules.").arg(passes.size()));
  RefreshViews();
}
void ReplayOverrides::RefreshViews()
{
  m_Render->update();
  if(m_Ctx.GetEventBrowser())
    for(QAbstractItemView *view :
        m_Ctx.GetEventBrowser()->Widget()->findChildren<QAbstractItemView *>())
      view->viewport()->update();
}

rdcstr ReplayOverrides::CameraState() const
{
  auto matrixArray = [](const QMatrix4x4 &matrix) {
    QJsonArray result;
    for(int r = 0; r < 4; r++)
      for(int c = 0; c < 4; c++)
        result.append(matrix(r, c));
    return result;
  };
  const bool loaded = m_Ctx.IsCaptureLoaded();
  const bool supported = loaded && m_Ctx.APIProps().pipelineType == GraphicsAPI::D3D11 &&
                         !m_Ctx.Replay().CurrentRemote().IsValid();
  QJsonObject result;
  result[lit("supported")] = supported;
  result[lit("semantics")] = QJsonArray::fromStringList(Semantics);
  result[lit("reason")] = supported ? QString() : tr("Requires a loaded local D3D11 capture.");
  result[lit("enabled")] = m_Enabled;
  result[lit("busy")] = m_Busy;
  result[lit("applying")] = m_Applying || m_StagingApply;
  result[lit("apply_phase")] = m_ApplyPhase;
  result[lit("last_update_ms")] = m_LastUpdateMS;
  result[lit("apply_timings_ms")] = m_ApplyTimings;
  result[lit("pending_update")] = m_Dirty || m_Applying || m_StagingApply;
  result[lit("last_error")] = m_LastCameraError;
  result[lit("scanning")] = bool(m_Scan);
  result[lit("scan_report")] = m_ScanReport->toPlainText();
  result[lit("recipe_supported")] = true;
  result[lit("recipe_versions")] = QJsonArray{1, 2};
  result[lit("recipe_passes_supported")] = true;
  result[lit("scan_pass_count")] = m_ScanResult[lit("pass_count")];
  result[lit("unresolved")] = m_Recipe[lit("unresolved")];
  result[lit("active_pass_count")] = int(m_Passes.size());
  if(m_ManagePasses) result[lit("passes")] = m_ConfigPasses;
  result[lit("recipe")] = m_Recipe;
  result[lit("recipe_result_ready")] = !m_Recipe.isEmpty() && !m_ScanResult.isEmpty();
  result[lit("scan_partial")] = m_ScanResult[lit("partial")];
  result[lit("scan_field_count")] = m_ScanResult[lit("config")].toObject()[lit("fields")].toArray().size();
  result[lit("saving_configuration")] = m_SidecarBusy;
  result[lit("configuration_status")] = m_Status->text();
  result[lit("panel_visible")] = m_Dialog->isVisible();
  result[lit("event_id")] = double(m_Ctx.CurEvent());
  result[lit("speed")] = m_Speed->value();
  result[lit("forward")] = m_Forward->currentIndex() == 0 ? lit("-Z") : lit("+Z");
  result[lit("world_up")] = m_WorldUp->currentText();
  result[lit("world_up_axes")] = QJsonArray::fromStringList(WorldUpAxes);
  QMatrix4x4 view, projection;
  result[lit("view")] = ParseMatrix(m_View->toPlainText(), view) ? matrixArray(view) : QJsonArray();
  result[lit("projection")] =
      ParseMatrix(m_Projection->toPlainText(), projection) ? matrixArray(projection) : QJsonArray();
  result[lit("current_view")] = matrixArray(m_Enabled ? m_Pose.inverted() : view);
  QJsonArray fields;
  for(int row = 0; row < m_CameraTable->rowCount(); row++)
  {
    QJsonObject f;
    f[lit("semantic")] = Semantics[ChoiceIndex(m_CameraTable, row, 0)];
    f[lit("first_event")] = double(Cell(m_CameraTable, row, 1).toUInt());
    f[lit("last_event")] = double(Cell(m_CameraTable, row, 2).toUInt());
    f[lit("shader")] = Cell(m_CameraTable, row, 3);
    f[lit("stage")] = Stages[ChoiceIndex(m_CameraTable, row, 4)];
    f[lit("slot")] = double(Cell(m_CameraTable, row, 5).toUInt());
    f[lit("offset")] = double(Cell(m_CameraTable, row, 6).toUInt());
    f[lit("row_major")] = ChoiceIndex(m_CameraTable, row, 7) == 0;
    f[lit("row_vector")] = ChoiceIndex(m_CameraTable, row, 8) == 1;
    QMatrix4x4 independentProjection;
    if(ParseMatrix(Cell(m_CameraTable, row, 9), independentProjection))
      f[lit("projection")] = matrixArray(independentProjection);
    QMatrix4x4 model;
    if(ChoiceIndex(m_CameraTable, row, 0) == 9 && ParseMatrix(Cell(m_CameraTable, row, 10), model))
      f[lit("model")] = matrixArray(model);
    fields.append(f);
  }
  result[lit("fields")] = fields;
  return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

rdcstr ReplayOverrides::ConfigureCamera(const rdcstr &configuration)
{
  if(!m_Ctx.IsCaptureLoaded() || m_Ctx.APIProps().pipelineType != GraphicsAPI::D3D11 ||
     m_Ctx.Replay().CurrentRemote().IsValid())
    return "Free camera requires a loaded local D3D11 capture.";
  if(m_Busy || m_Dirty || m_Scan || m_Applying || m_StagingApply)
    return "Camera replay is busy; wait for busy/pending_update to clear and retry.";
  QJsonParseError parseError;
  QJsonDocument doc = QJsonDocument::fromJson(QString(configuration).toUtf8(), &parseError);
  if(parseError.error != QJsonParseError::NoError || !doc.isObject())
    return "Camera configuration must be a JSON object.";
  const QJsonObject config = doc.object();
  if(config.contains(lit("apply_recipe_result")) && !config[lit("apply_recipe_result")].isBool())
    return "apply_recipe_result must be a boolean.";
  if(config[lit("apply_recipe_result")].toBool())
  {
    if(m_Recipe.isEmpty() || m_ScanResult.isEmpty())
      return "No completed recipe result is available.";
    ApplyScanResult();
    return {};
  }
  if(config.contains(lit("recipe")) && !config.contains(lit("fields")))
  {
    if(!config[lit("recipe")].isObject())
      return "recipe must be a JSON object.";
    if(QJsonDocument(config[lit("recipe")].toObject()).toJson(QJsonDocument::Compact).size() > 1024 * 1024)
      return "Camera recipe exceeds the 1 MiB limit.";
    const QJsonObject recipe = config[lit("recipe")].toObject();
    if(!recipe.isEmpty() && ((recipe[lit("version")].toInt() != 1 && recipe[lit("version")].toInt() != 2) ||
       !recipe[lit("camera")].isObject() || !recipe[lit("layouts")].isArray()))
      return "Recipe needs version 1 or 2, camera locators and layouts.";
    if(config.contains(lit("run_recipe")) && !config[lit("run_recipe")].isBool())
      return "run_recipe must be a boolean.";
    if(config.contains(lit("show")) && !config[lit("show")].isBool())
      return "show must be a boolean.";
    if(config[lit("run_recipe")].toBool() && recipe.isEmpty())
      return "Cannot run an empty recipe.";
    ++m_ConfigRevision;
    m_Recipe = recipe;
    m_ScanResult = QJsonObject();
    m_ApplyPartial->setEnabled(false);
    m_ScanReport->setPlainText(recipe.isEmpty() ? tr("Recipe cleared.") :
        tr("Recipe loaded. Run it to validate the layout and expand fields for this capture."));
    m_AutoConfigure->setText(recipe.isEmpty() ? tr("Auto configure") : tr("Auto configure"));
    if(config[lit("show")].toBool(true)) ShowCamera();
    if(config[lit("run_recipe")].toBool()) StartCameraScan();
    return {};
  }
  auto readMatrix = [](const QJsonValue &value, QMatrix4x4 &matrix) {
    if(!value.isArray() || value.toArray().size() != 16)
      return false;
    QJsonArray array = value.toArray();
    for(int i = 0; i < 16; i++)
    {
      double v = array[i].toDouble();
      if(!array[i].isDouble() || !std::isfinite(v) || !std::isfinite(float(v)))
        return false;
      matrix(i / 4, i % 4) = float(v);
    }
    bool invertible = false;
    matrix.inverted(&invertible);
    return invertible;
  };
  QMatrix4x4 view, projection;
  if(!readMatrix(config[lit("view")], view) || !readMatrix(config[lit("projection")], projection))
    return "View and projection require 16 finite, invertible mathematical row-ordered values.";
  if(!config[lit("fields")].isArray() || (config[lit("fields")].toArray().isEmpty() && !m_RestoringForm) ||
     config[lit("fields")].toArray().size() > 65536)
    return "Camera fields must contain 1 to 65536 entries.";
  auto readUInt = [](const QJsonObject &obj, const QString &key, uint32_t &value) {
    QJsonValue v = obj[key];
    double n = v.toDouble(-1);
    if(!v.isDouble() || n < 0 || n > 4294967295.0 || std::floor(n) != n)
      return false;
    value = uint32_t(n);
    return true;
  };
  rdcarray<CameraField> fields;
  for(const QJsonValue &value : config[lit("fields")].toArray())
  {
    if(!value.isObject())
      return "Each camera field must be an object.";
    QJsonObject obj = value.toObject();
    CameraField field;
    field.semantic = Semantics.indexOf(obj[lit("semantic")].toString());
    int stage = Stages.indexOf(obj[lit("stage")].toString().toUpper());
    field.patch.shader = LookupResource(obj[lit("shader")].toString());
    const bool shaderResource = m_ShaderNames.contains(ToQStr(field.patch.shader));
    if(field.semantic < 0 || stage < 0 || !shaderResource ||
       !readUInt(obj, lit("first_event"), field.patch.firstEvent) ||
       !readUInt(obj, lit("last_event"), field.patch.lastEvent) ||
       !readUInt(obj, lit("slot"), field.patch.slot) ||
       !readUInt(obj, lit("offset"), field.patch.byteOffset) || field.patch.firstEvent == 0 ||
       field.patch.firstEvent > field.patch.lastEvent ||
       field.patch.lastEvent > m_Ctx.GetLastAction()->eventId || field.patch.slot >= 14 ||
       field.patch.byteOffset > uint32_t(65536 - (field.semantic == 6 ? 12 : 64)))
      return "Invalid camera field: semantic, shader, stage, event range, CB slot or byte offset.";
    field.patch.stage = ShaderStage(stage);
    if((obj.contains(lit("row_major")) && !obj[lit("row_major")].isBool()) ||
       (obj.contains(lit("row_vector")) && !obj[lit("row_vector")].isBool()))
      return "row_major and row_vector must be booleans.";
    field.rowMajor = obj[lit("row_major")].toBool(true);
    field.rowVector = obj[lit("row_vector")].toBool(false);
    field.hasProjection = obj.contains(lit("projection"));
    if(field.hasProjection && !readMatrix(obj[lit("projection")], field.projection))
      return "Field projection requires 16 finite, invertible values.";
    if(field.semantic == 9 && !readMatrix(obj[lit("model")], field.model))
      return "ModelView requires a finite, invertible model matrix.";
    fields.push_back(field);
  }
  double speed = config[lit("speed")].toDouble(m_Speed->value());
  QString forward = config[lit("forward")].toString(lit("-Z"));
  const QString worldUp = config.contains(lit("world_up")) ? config[lit("world_up")].toString() : lit("+Y");
  if(!WorldUpAxes.contains(worldUp)) return "world_up must be +X, -X, +Y, -Y, +Z or -Z.";
  if((config.contains(lit("speed")) && !config[lit("speed")].isDouble()) || !std::isfinite(speed) ||
     speed < 0.0001 || speed > 1000000 || (forward != lit("-Z") && forward != lit("+Z")))
    return "Invalid camera speed or forward axis (-Z/+Z).";
  for(const QString &key : {lit("enabled"), lit("show")})
    if(config.contains(key) && !config[key].isBool())
      return "enabled/show must be booleans.";
  if(config.contains(lit("flip_y")) && !config[lit("flip_y")].isBool())
    return "flip_y must be a boolean.";
  if(config.contains(lit("output_rid")))
  {
    bool found = false;
    for(const TextureDescription &texture : m_Ctx.GetTextures())
      found |= ToQStr(texture.resourceId) == config[lit("output_rid")].toString();
    if(!found)
      return "Saved output texture does not exist in this capture.";
  }
  uint32_t event = 0;
  if(config.contains(lit("event_id")) &&
     (!readUInt(config, lit("event_id"), event) || !m_Ctx.GetAction(event)))
    return "Saved display event does not exist in this capture.";

  rdcarray<ReplayPassOverride> passes;
  if(config.contains(lit("passes")))
  {
    rdcstr error = DecodePasses(config[lit("passes")], passes);
    if(!error.empty()) return error;
  }
  // Validate everything before changing the visible form. The existing camera path owns replay.
  if(config[lit("enabled")].toBool(true) && !m_RestoringForm)
    m_BeforeApplyForm = QJsonDocument::fromJson(QString(CameraState()).toUtf8()).object();
  ++m_ConfigRevision;
  m_Recipe = config[lit("recipe")].toObject();
  m_ManagePasses = config.contains(lit("passes"));
  m_ConfigPasses = config[lit("passes")].toArray();
  if(m_ManagePasses) PopulatePasses(passes);
  m_AutoConfigure->setText(m_Recipe.isEmpty() ? tr("Auto configure") : tr("Auto configure"));
  m_DisplayConfig = QJsonObject();
  for(const QString &key : {lit("output_rid"), lit("flip_y"), lit("event_id")})
    if(config.contains(key))
      m_DisplayConfig[key] = config[key];
  {
    // Filling hundreds of rows must not repeatedly resize every column or repaint each widget.
    const QSignalBlocker blocker(m_CameraTable);
    m_CameraTable->setUpdatesEnabled(false);
    m_CameraTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_CameraTable->setRowCount(0);
    for(const CameraField &field : fields)
      AddCameraField(field);
    m_CameraTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_CameraTable->setUpdatesEnabled(true);
  }
  m_View->setPlainText(MatrixText(view));
  m_Projection->setPlainText(MatrixText(projection));
  m_Speed->setValue(speed);
  m_Forward->setCurrentIndex(forward == lit("-Z") ? 0 : 1);
  m_WorldUp->setCurrentIndex(WorldUpAxes.indexOf(worldUp));
  if(config[lit("enabled")].toBool(true))
    EnableCamera();
  else if(m_Enabled)
    ResetCamera();
  if(config[lit("show")].toBool(true))
    ShowCamera();
  return {};
}

void ReplayOverrides::StartCameraScan()
{
  if(m_Scan || m_Busy || m_Dirty || m_Applying || m_StagingApply || !m_Ctx.IsCaptureLoaded() ||
     m_Ctx.APIProps().pipelineType != GraphicsAPI::D3D11 || m_Ctx.Replay().CurrentRemote().IsValid())
  {
    Error(tr("Wait for the current replay operation to finish."));
    return;
  }
  if(m_Recipe.isEmpty())
  {
    m_ScanReport->setPlainText(tr("Import a configuration JSON, or ask an agent to analyse this capture and fill the recipe."));
    return;
  }
  m_Scan = std::make_shared<CameraScan>();
  m_LastCameraError.clear();
  m_Scan->capturePath = QString(m_Ctx.GetCaptureFilename());
  m_Scan->recipe = m_Recipe;
  m_ScanResult = QJsonObject();
  m_ScanProgress->setRange(0, 0);
  ++m_ConfigRevision;    // A late sidecar read must not replace a user's scan/configuration.
  m_Keys.clear();
  m_Dragging = false;
  m_AutoConfigure->setEnabled(false);
  m_CancelScan->setEnabled(true);
  m_ApplyPartial->setEnabled(false);
  m_ScanReport->setPlainText(tr("Checking the selected draw and scanning camera bindings..."));
  if(!m_Recipe.isEmpty())
    m_ScanReport->setPlainText(tr("Validating recipe and expanding its camera fields..."));
  StepCameraScan();
}

void ReplayOverrides::EditCameraRecipe()
{
  if(m_Scan || m_Busy || m_Dirty || m_Applying || m_StagingApply)
  {
    Error(tr("Wait for the current camera operation to finish."));
    return;
  }
  QDialog dialog(m_Dialog);
  dialog.setWindowTitle(tr("Replay recipe (JSON)"));
  dialog.resize(820, 650);
  QVBoxLayout *layout = new QVBoxLayout(&dialog);
  QLabel *help = new QLabel(tr("Describe verified offsets and meanings. The script reads matrices and expands "
      "shader/event bindings. Offsets are relative to the bound CB range.\n"
      "Full recipes retain capture fingerprints and pass rules. Camera layout export removes both; reanalyse each new capture. "
      "Manual edits to the generated field table do not change the recipe."));
  help->setWordWrap(true);
  layout->addWidget(help);
  QPlainTextEdit *editor = new QPlainTextEdit(&dialog);
  editor->setObjectName(lit("replayCameraRecipeJSON"));
  QJsonObject initial = m_Recipe;
  if(initial.isEmpty())
    initial = QJsonDocument::fromJson(R"({"version":2,"passes":[],"name":"Camera layout","row_major":true,"row_vector":true,"forward":"-Z","world_up":"+Y","camera":{"view":{"slot":0,"offset":0},"inverse_view":{"slot":0,"offset":64},"projection":{"slot":0,"offset":128}},"layouts":[{"slot":0,"stages":["VS","PS","CS"],"fields":[{"semantic":"View","offset":0},{"semantic":"InverseView","offset":64}]}],"unresolved":["Fill and verify this layout before use."]})").object();
  editor->setPlainText(QString::fromUtf8(QJsonDocument(initial).toJson()));
  layout->addWidget(editor);
  QLabel *error = new QLabel(&dialog);
  error->setWordWrap(true);
  layout->addWidget(error);
  auto read = [editor, error](QJsonObject &recipe) {
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(editor->toPlainText().toUtf8(), &parse);
    if(parse.error != QJsonParseError::NoError || !doc.isObject())
    {
      error->setText(tr("Recipe must be a JSON object: %1").arg(parse.errorString()));
      return false;
    }
    recipe = doc.object();
    return true;
  };
  QHBoxLayout *buttons = new QHBoxLayout;
  layout->addLayout(buttons);
  Button(buttons, tr("Import JSON..."), [&dialog, editor, error]() {
    QString path = QFileDialog::getOpenFileName(&dialog, tr("Import camera recipe"), QString(), tr("JSON (*.json)"));
    if(path.isEmpty()) return;
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) { error->setText(file.errorString()); return; }
    if(file.size() > 1024 * 1024) { error->setText(tr("Recipe exceeds 1 MiB.")); return; }
    editor->setPlainText(QString::fromUtf8(file.readAll()));
  });
  Button(buttons, tr("Export full recipe..."), [&dialog, read, error]() {
    QJsonObject recipe;
    if(!read(recipe)) return;
    QString path = QFileDialog::getSaveFileName(&dialog, tr("Export full recipe"), QString(), tr("JSON (*.json)"));
    if(path.isEmpty()) return;
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(recipe).toJson();
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
      error->setText(file.errorString());
  });
  Button(buttons, tr("Export camera layout..."), [&dialog, read, error]() {
    QJsonObject recipe;
    if(!read(recipe)) return;
    QString path = QFileDialog::getSaveFileName(&dialog, tr("Export camera recipe"), QString(), tr("JSON (*.json)"));
    if(path.isEmpty()) return;
    recipe.remove(lit("reference_event"));
    recipe.remove(lit("capture"));
    if(recipe[lit("version")].toInt() == 2) recipe[lit("passes")] = QJsonArray();
    QSaveFile file(path);
    QByteArray bytes = QJsonDocument(recipe).toJson();
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
      error->setText(file.errorString());
  });
  auto use = [this, &dialog, read, error](bool run) {
    QJsonObject recipe;
    if(!read(recipe)) return;
    QJsonObject request;
    request[lit("recipe")] = recipe;
    request[lit("run_recipe")] = run;
    request[lit("show")] = false;
    rdcstr message = ConfigureCamera(QString::fromUtf8(QJsonDocument(request).toJson()));
    if(!message.empty()) error->setText(QString(message));
    else dialog.accept();
  };
  Button(buttons, tr("Use recipe"), [use]() { use(false); });
  Button(buttons, tr("Use and run"), [use]() { use(true); });
  Button(buttons, tr("Cancel"), [&dialog]() { dialog.reject(); });
  dialog.exec();
}

void ReplayOverrides::CancelCameraScan()
{
  if(m_Scan)
  {
    // Keep the session until its queued batch restores replay. No new scan can overtake it.
    m_Scan->cancelled = true;
    m_CancelScan->setEnabled(false);
  }
}

static void RunCameraScanBatch(const std::shared_ptr<CameraScan> &scan, IReplayController *r,
                               const rdcarray<ReplayConstantOverride> &patches,
                               const rdcarray<ReplayPassOverride> &passes, uint32_t event,
                               QString &output, QString &error)
{
  // Output is emitted both by the Python output timer and at executeString completion.
  QMutex mutex;
  auto out = ::QObject::connect(
      scan->python, &PythonContext::textOutput, scan->python,
      [&](bool stderrOutput, const QString &text) {
        QMutexLocker lock(&mutex);
        (stderrOutput ? error : output) += text;
      },
      Qt::DirectConnection);
  auto exception = ::QObject::connect(
      scan->python, &PythonContext::exception, scan->python,
      [&](const QString &type, const QString &value, int, QList<QString>) {
        QMutexLocker lock(&mutex);
        error += type + lit(": ") + value;
      },
      Qt::DirectConnection);
  if(!scan->cancelled)
  {
    rdcstr replayError = r->SetReplayOverrides({}, {}, event);
    if(!replayError.empty())
      error = QString(replayError);
    else
    {
      if(!scan->started)
      {
        if(scan->recipe[lit("reference_event")].toDouble() > 0 || !scan->recipe[lit("passes")].toArray().isEmpty())
        {
          QFile capture(scan->capturePath);
          const QFileInfo before(scan->capturePath);
          QCryptographicHash hash(QCryptographicHash::Sha256);
          if(!capture.open(QIODevice::ReadOnly)) error = capture.errorString();
          else
          {
            while(!capture.atEnd() && !scan->cancelled)
            {
              const QByteArray bytes = capture.read(1024 * 1024);
              if(bytes.isEmpty()) { error = lit("Could not read capture fingerprint."); break; }
              hash.addData(bytes);
            }
            scan->fingerprint[lit("size")] = double(capture.size());
            scan->fingerprint[lit("sha256")] = QString::fromLatin1(hash.result().toHex());
            const QFileInfo after(scan->capturePath);
            if(before.size() != after.size() || before.lastModified() != after.lastModified())
              error = lit("Capture changed while calculating its fingerprint.");
          }
        }
        QFile common(lit(":/freecamera/freecamera_common.py"));
        QFile script(lit(":/freecamera/freecamera_recipe.py"));
        if(!common.open(QIODevice::ReadOnly) || !script.open(QIODevice::ReadOnly))
          error = lit("Bundled recipe scripts are missing.");
        if(error.isEmpty() && !scan->cancelled)
        {
          scan->python->setGlobal("controller", r);
          auto quote = [](const QString &text) {
            QByteArray value = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
            return QString::fromUtf8(value.mid(1, value.size() - 2));
          };
          const QString recipeJSON = QString::fromUtf8(QJsonDocument(scan->recipe).toJson(QJsonDocument::Compact));
          const QString fingerprintJSON = QString::fromUtf8(QJsonDocument(scan->fingerprint).toJson(QJsonDocument::Compact));
          const QString program = lit("import types, sys\n_camera_math = types.ModuleType('freecamera_common')\nexec(%1, _camera_math.__dict__)\nsys.modules['freecamera_common'] = _camera_math\n").arg(quote(QString::fromUtf8(common.readAll()))) +
              QString::fromUtf8(script.readAll()) +
              lit("\nscanner = RecipeScanner(controller, json.loads(%1), json.loads(%2))\n").arg(quote(recipeJSON), quote(fingerprintJSON));
          scan->python->executeString(lit("freecamera_recipe.py"), program);
        }
        scan->started = true;
      }
      if(error.isEmpty() && !scan->cancelled)
        scan->python->executeString(lit("print(json.dumps(scanner.step(16)))\n"));
    }
    replayError = r->SetReplayOverrides(patches, passes, event);
    if(!replayError.empty())
      error += QString(replayError);
  }
  ::QObject::disconnect(out);
  ::QObject::disconnect(exception);
}

void ReplayOverrides::StepCameraScan()
{
  const auto scan = m_Scan;
  const auto patches = m_AppliedPatches;
  const auto passes = m_Passes;
  const uint32_t event = m_Ctx.CurEvent();
  const uint64_t epoch = m_Epoch;
  QPointer<ReplayOverrides> weak(this);
  m_Ctx.Replay().AsyncInvoke([weak, scan, patches, passes, event, epoch](IReplayController *r) {
    QString output, error;
    RunCameraScanBatch(scan, r, patches, passes, event, output, error);
    if(weak)
      GUIInvoke::call(weak, [weak, scan, output, error, epoch]() {
        if(!weak || weak->m_Epoch != epoch || weak->m_Scan != scan)
          return;
        const QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8());
        const QJsonObject result = doc.object();
        const bool failed = !error.isEmpty() || !doc.isObject();
        if(scan->cancelled || failed || result[lit("done")].toBool())
        {
          weak->m_Scan.reset();
          weak->m_AutoConfigure->setEnabled(true);
          weak->m_CancelScan->setEnabled(false);
          if(scan->cancelled || failed)
          {
            if(failed && !scan->cancelled)
              weak->m_LastCameraError = error.isEmpty() ? output : error;
            weak->m_ScanReport->setPlainText(scan->cancelled
                                                 ? tr("Scan cancelled. Configuration unchanged.")
                                                 : tr("Scan failed. Configuration unchanged.\n%1")
                                                       .arg(error.isEmpty() ? output : error));
            return;
          }
          weak->m_ScanResult = result;
          weak->m_ScanReport->setPlainText(result[lit("report")].toString());
          if(result[lit("partial")].toBool())
            weak->m_ApplyPartial->setEnabled(true);
          else
            weak->ApplyScanResult();
        }
        else
          weak->StepCameraScan();
        weak->m_ScanProgress->setRange(0, result[lit("total")].toInt());
        weak->m_ScanProgress->setValue(result[lit("progress")].toInt());
        if(!result[lit("phase")].toString().isEmpty() && !result[lit("done")].toBool())
          weak->m_ScanReport->setPlainText(result[lit("phase")].toString());
      });
  });
}

void ReplayOverrides::ApplyScanResult()
{
  if(m_Scan || m_ScanResult.isEmpty() || m_StagingApply || m_Applying || m_Busy || m_Dirty)
    return;
  QJsonObject config = m_ScanResult[lit("config")].toObject();
  config[lit("speed")] = m_Speed->value();
  if(m_ScanResult.contains(lit("event_id")))
    config[lit("event_id")] = m_ScanResult[lit("event_id")];
  m_StagingApply = true;
  m_Keys.clear();
  m_Dragging = false;
  SetApplyControls(true);
  SetApplyPhase(lit("preparing"));
  const uint64_t epoch = m_Epoch;
  QTimer::singleShot(0, this, [this, config, epoch]() {
    if(epoch != m_Epoch || !m_StagingApply)
      return;
    m_StagingApply = false;
    SetApplyControls(false);
    rdcstr error = m_Ctx.GetTextureViewer()->ConfigureReplayCamera(
        QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact)));
    if(!error.empty())
    {
      m_ApplyProgress->hide();
      m_ApplyPhase = lit("failed");
      m_ApplyStatus->setText(QString(error));
      Error(QString(error));
    }
    else
      m_ApplyPartial->setEnabled(false);
  });
}

void ReplayOverrides::SaveCamera(const QJsonObject &configuration)
{
  // Persist baseline configuration, including explicitly managed passes, never the moving pose.
  QJsonObject config;
  for(const QString &key :
      {lit("view"), lit("projection"), lit("fields"), lit("speed"), lit("forward"), lit("world_up"), lit("recipe"), lit("passes")})
    if(configuration.contains(key)) config[key] = configuration[key];
  QJsonObject display =
      QJsonDocument::fromJson(QString(m_Ctx.GetTextureViewer()->GetReplayCameraState()).toUtf8())
          .object();
  config[lit("flip_y")] = display[lit("flip_y")];
  for(const TextureDescription &texture : m_Ctx.GetTextures())
    if(ToQStr(texture.resourceId) == display[lit("output_rid")].toString())
      config[lit("output_rid")] = display[lit("output_rid")];
  if(m_Ctx.GetAction(m_Ctx.CurEvent()))
    config[lit("event_id")] = double(m_Ctx.CurEvent());
  m_PendingSave = config;
  if(!m_SidecarBusy)
    CameraSidecarIO(false);
}

void ReplayOverrides::ReadCameraSidecar()
{
  if(QFile::exists(QString(m_Ctx.GetCaptureFilename()) + lit(".freecamera.json")))
    CameraSidecarIO(true);
}

void ReplayOverrides::CameraSidecarIO(bool load)
{
  m_SidecarBusy = true;
  const uint64_t epoch = m_Epoch, revision = m_ConfigRevision;
  const QString path = QString(m_Ctx.GetCaptureFilename());
  const QJsonObject config = m_PendingSave;
  if(!load)
    m_PendingSave = QJsonObject();
  QPointer<ReplayOverrides> weak(this);
  LambdaThread *thread = new LambdaThread([weak, path, epoch, revision, config, load]() {
    QString error;
    QJsonObject saved, fingerprint;
    QFile capture(path);
    const QFileInfo before(path);
    if(!capture.open(QIODevice::ReadOnly))
      error = capture.errorString();
    else
    {
      QCryptographicHash hash(QCryptographicHash::Sha256);
      if(!hash.addData(&capture))
        error = lit("Could not hash the complete capture.");
      else
      {
        fingerprint[lit("size")] = double(capture.size());
        fingerprint[lit("sha256")] = QString::fromLatin1(hash.result().toHex());
      }
      const QFileInfo after(path);
      if(before.size() != after.size() || before.lastModified() != after.lastModified())
        error = lit("Capture changed while calculating its fingerprint.");
    }
    if(load && error.isEmpty())
    {
      QFile sidecar(path + lit(".freecamera.json"));
      if(!sidecar.open(QIODevice::ReadOnly))
        error = sidecar.errorString();
      else if(sidecar.size() > 64 * 1024 * 1024)
        error = lit("Camera configuration is too large.");
      else
      {
        QJsonParseError parse;
        QJsonDocument doc = QJsonDocument::fromJson(sidecar.readAll(), &parse);
        if(parse.error != QJsonParseError::NoError || !doc.isObject())
          error = lit("Invalid camera configuration JSON.");
        else
        {
          saved = doc.object();
          if(saved[lit("version")].toInt() != 1 && saved[lit("version")].toInt() != 2 && saved[lit("version")].toInt() != 3)
            error = lit("Unsupported camera configuration version.");
          else if(saved[lit("capture")].toObject() != fingerprint)
            error = lit("Camera configuration belongs to a different capture.");
        }
      }
    }
    if(weak)
      GUIInvoke::call(weak, [weak, epoch, revision, path, config, saved, fingerprint, error, load]() {
        if(!weak || weak->m_Epoch != epoch)
          return;
        weak->m_SidecarBusy = false;
        QString failure = error;
        if(load && weak->m_ConfigRevision == revision && failure.isEmpty())
        {
          QJsonObject restored = saved[lit("configuration")].toObject();
          // Old sidecars already authenticate their capture. Supply that fingerprint to
          // legacy recipes that predate the recipe-level capture field.
          QJsonObject recipe = restored[lit("recipe")].toObject();
          if(recipe[lit("reference_event")].toDouble() > 0 && !recipe.contains(lit("capture")))
          {
            recipe[lit("capture")] = saved[lit("capture")];
            restored[lit("recipe")] = recipe;
          }
          restored[lit("enabled")] = false;
          restored[lit("show")] = false;
          failure =
              QString(weak->ConfigureCamera(QString::fromUtf8(QJsonDocument(restored).toJson())));
          if(failure.isEmpty())
            weak->Error(
                tr("Configuration restored. Camera and pass overrides remain disabled until you enable them."));
        }
        else if(!load && failure.isEmpty())
        {
          QJsonObject document;
          document[lit("version")] = 3;
          document[lit("capture")] = fingerprint;
          document[lit("configuration")] = config;
          QSaveFile file(path + lit(".freecamera.json"));
          const QByteArray bytes = QJsonDocument(document).toJson();
          if(!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            failure = file.errorString();
          else
          {
            weak->m_SaveStatus->clear();
            weak->Error(tr("Configuration saved beside the RDC."));
          }
        }
        if(!failure.isEmpty())
        {
          if(!load)
          {
            weak->m_SaveStatus->setText(tr("Configuration not saved"));
            weak->m_SaveStatus->setToolTip(failure);
          }
          weak->Error(
              load ? tr("Configuration not restored: %1. Run auto configure again.").arg(failure)
                   : tr("Camera works in this session, but configuration was not saved: %1")
                         .arg(failure));
        }
        if(!weak->m_PendingSave.isEmpty())
          weak->CameraSidecarIO(false);
      });
  });
  thread->setName(lit("Free camera configuration"));
  thread->selfDelete(true);
  thread->start();
}
