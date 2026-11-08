#pragma once
#include <QImage>
#include <QMainWindow>
#include <QMap>
#include "PathPlanner.h"
#include "Types.h"

class CanvasView;
class PlotterLink;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;
class QTimer;
class QFontComboBox;
class QListWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

private slots:
    void toggleConnect();
    void refreshPorts();
    void openFile();
    void saveSvg();
    void replan();
    void plot();
    void uploadJob();
    void loadSettings();
    void refreshFiles();

private:
    QWidget* buildConnectionPanel();
    QWidget* buildControlPanel();
    QWidget* buildJobTab();
    QWidget* buildSettingsTab();
    QWidget* buildTerminalTab();
    QWidget* buildFlashTab();
    QWidget* buildCalibTab();
    QWidget* buildTextTab();
    void applyText();
    QWidget* saveBar();
    void syncCalib();
    void saveCalibrationRecord(double w, double h);   // tulis data kalibrasi manual ke calibration_data.jsonl
    double cfg(const QString& k, double def = 0) const { return cfg_.value(k, def); }
    void retrace();
    void applySource(const Paths& p, const QString& info, bool keepSize = false);
    void onReply(const QString& cmd, const QStringList& data, const QString& status);
    void jog(const QString& dir);
    QStringList jobCommands() const;

    PlotterLink* link_;
    CanvasView* canvas_;
    // koneksi
    QComboBox *mode_, *port_, *baud_;
    QLineEdit* host_;
    QSpinBox* tcpPort_;
    QPushButton* connectBtn_;
    QLabel* statusLbl_;
    // kontrol
    QDoubleSpinBox* step_;
    QDoubleSpinBox *travelV_ = nullptr, *drawV_ = nullptr;
    QTimer* speedTimer_ = nullptr;
    // job
    QLabel* srcInfo_;
    QSlider* thr_;
    QCheckBox *invert_, *flip_, *optimize_, *mirrorX_ = nullptr, *mirrorY_ = nullptr;
    QDoubleSpinBox *areaW_, *areaH_, *margin_, *scale_, *offX_, *offY_;
    QProgressBar* progress_;
    QLabel* planInfo_;
    QPushButton *plotBtn_, *saveSvgBtn_;
    QLineEdit* jobName_;
    // setelan
    QTableWidget* settings_;
    bool loadingSettings_ = false;
    // terminal
    QPlainTextEdit* log_;
    QLineEdit* cmdLine_;
    // kalibrasi
    QSlider* svSlider_ = nullptr;
    QSpinBox *svAngle_ = nullptr, *svMinUs_ = nullptr, *svMaxUs_ = nullptr, *penMs_ = nullptr;
    QLabel *svUpLbl_ = nullptr, *svDnLbl_ = nullptr, *limLbl_ = nullptr, *spmmLbl_ = nullptr;
    QDoubleSpinBox *hFast_ = nullptr, *hOff_ = nullptr, *hSearch_ = nullptr, *measured_ = nullptr, *testSize_ = nullptr;
    QComboBox *hdx_ = nullptr, *hdy_ = nullptr;
    QCheckBox *hCenter_ = nullptr, *invA_ = nullptr, *invB_ = nullptr;
    QTimer* angleTimer_ = nullptr;
    // kalibrasi manual
    double curX_ = 0, curY_ = 0;
    bool haveStatus_ = false, curBusy_ = true;
    double edge_[4] = {0, 0, 0, 0};      // kiri, kanan, bawah, atas (mm, koordinat saat ditandai)
    bool edgeSet_[4] = {false, false, false, false};
    QLabel *edgeLbl_[4] = {nullptr, nullptr, nullptr, nullptr}, *posLbl_ = nullptr, *manInfo_ = nullptr;
    QMap<QString, double> cfg_;
    // teks
    QPlainTextEdit* textEdit_ = nullptr;
    QComboBox *textKind_ = nullptr, *strokeFont_ = nullptr, *textAlign_ = nullptr;
    QFontComboBox* sysFont_ = nullptr;
    QCheckBox *textBold_ = nullptr, *textItalic_ = nullptr, *hatch_ = nullptr, *autoInc_ = nullptr;
    QSpinBox *passes_ = nullptr, *counter_ = nullptr;
    QDoubleSpinBox *textH_ = nullptr, *textLs_ = nullptr, *textLine_ = nullptr, *textRot_ = nullptr, *hatchSp_ = nullptr, *hatchAng_ = nullptr, *penW_ = nullptr;
    QLabel* textInfo_ = nullptr;
    QTimer* textTimer_ = nullptr;
    bool keepSize_ = false, counterPending_ = false;
    // flash
    QListWidget* files_;
    QLabel* fsInfo_;

    // sumber gambar
    Paths src_;            // koordinat sumber (SVG unit / piksel)
    QRectF srcBounds_;
    QImage raster_;
    bool fromRaster_ = false;
    PlanResult plan_;
    QStringList pendingUpload_;
};
