#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QMatrix4x4>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <memory>
#include "Interface/QRDInterface.h"

class QDialog;
class QTableWidget;
class QPlainTextEdit;
class QDoubleSpinBox;
class QComboBox;
class QLabel;
class QPushButton;
class QProgressBar;
struct CameraScan;

// One session editor per capture context, owned by the texture viewer. No saved capture edits.
class ReplayOverrides : public QObject, public ICaptureViewer
{
public:
  ReplayOverrides(ICaptureContext &ctx, QWidget *parent, QWidget *render,
                  std::function<void(const QJsonObject &)> display);
  ~ReplayOverrides();
  static ReplayOverrides *Find(ICaptureContext &ctx);
  static QString PassLabel(ICaptureContext &ctx, uint32_t eventId);
  void ShowCamera();
  void ShowPass(uint32_t first, uint32_t last, bool add);
  QWidget *Toolbar() { return m_Toolbar; }
  rdcstr CameraState() const;
  rdcstr ConfigureCamera(const rdcstr &configuration);
  void ResetCamera();
  void OnCaptureLoaded() override;
  void OnCaptureClosed() override;
  void OnSelectedEventChanged(uint32_t) override {}
  void OnEventChanged(uint32_t) override;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  struct CameraField
  {
    ReplayConstantOverride patch;
    int semantic = 0;
    bool rowMajor = true, rowVector = false;
    bool hasProjection = false;
    QMatrix4x4 projection;
    QMatrix4x4 model;
  };
  static QMap<ICaptureContext *, ReplayOverrides *> s_Editors;
  ICaptureContext &m_Ctx;
  std::function<void(const QJsonObject &)> m_ApplyDisplay;
  QWidget *m_Render, *m_Toolbar;
  QDialog *m_Dialog;
  QTableWidget *m_CameraTable, *m_PassTable;
  QPlainTextEdit *m_View, *m_Projection;
  QDoubleSpinBox *m_Speed;
  QComboBox *m_Forward, *m_WorldUp;
  QLabel *m_Status;
  QLabel *m_CameraState;
  QString m_LastCameraError;
  QPushButton *m_Enable;
  QPushButton *m_AutoConfigure, *m_CancelScan, *m_ApplyPartial;
  QProgressBar *m_ScanProgress;
  QProgressBar *m_ApplyProgress;
  QLabel *m_ApplyStatus, *m_SaveStatus;
  QList<QWidget *> m_ApplyControls;
  QMap<QWidget *, bool> m_ApplyControlStates;
  bool m_Applying = false;
  bool m_StagingApply = false;
  QString m_ApplyPhase;
  QElapsedTimer m_ApplyClock;
  double m_LastUpdateMS = 0;
  QJsonObject m_ApplyTimings;
  uint32_t m_TargetEvent = 0;
  uint64_t m_Submission = 0;
  QPlainTextEdit *m_ScanReport;
  std::shared_ptr<CameraScan> m_Scan;
  QJsonObject m_ScanResult, m_DisplayConfig, m_SaveAfterApply, m_PendingSave;
  QJsonObject m_Recipe;
  QJsonArray m_ConfigPasses;
  bool m_ManagePasses = false, m_AppliedManagePasses = false;
  std::function<void()> m_RestoreCamera;
  QJsonObject m_BeforeApplyForm;
  bool m_RestoringForm = false;
  rdcarray<ReplayConstantOverride> m_AppliedPatches;
  bool m_SidecarBusy = false;
  uint64_t m_ConfigRevision = 0;
  QHash<QString, ResourceId> m_ResourceLookup;
  QSet<QString> m_ShaderNames;
  size_t m_ResourceLookupSize = 0;
  QTimer m_Timer;
  QElapsedTimer m_Clock;
  QSet<int> m_Keys;
  QPoint m_Mouse;
  bool m_Dragging = false, m_Enabled = false, m_Busy = false, m_Dirty = false;
  uint64_t m_Epoch = 0;
  QMatrix4x4 m_Proj, m_Pose, m_WorldFrame;
  QVector3D m_Position;
  float m_Yaw = 0, m_Pitch = 0;
  float m_Handedness = 1.0f;
  float m_ForwardDirection = -1.0f;
  rdcarray<CameraField> m_Fields;
  rdcarray<ReplayPassOverride> m_Passes;
  void AddCameraField(const CameraField &field);
  void AddReflectedField();
  bool ReadCameraFields(rdcarray<CameraField> &fields);
  void ReadBaseline();
  void EnableCamera();
  void PrepareCamera();
  void SetApplyPhase(const QString &phase);
  void SetApplyControls(bool disabled);
  void FinishApply(const QString &error);
  void Tick();
  void WatchInput(QWidget *widget);
  void UpdatePose();
  float HorizontalInputSign() const;
  void SubmitCamera();
  void AddPassRow(uint32_t first, uint32_t last);
  void ApplyPasses();
  rdcstr DecodePasses(const QJsonValue &value, rdcarray<ReplayPassOverride> &passes);
  void PopulatePasses(const rdcarray<ReplayPassOverride> &passes);
  void RefreshViews();
  ResourceId LookupResource(const QString &text);
  void Error(const QString &text);
  void StartCameraScan();
  void EditCameraRecipe();
  void StepCameraScan();
  void CancelCameraScan();
  void ApplyScanResult();
  void SaveCamera(const QJsonObject &config);
  void ReadCameraSidecar();
  void CameraSidecarIO(bool load);
};
