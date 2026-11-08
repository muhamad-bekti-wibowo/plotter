#include "MainWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include "GcodeImport.h"
#include "TextRender.h"
#include <QFontComboBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <algorithm>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QDateTime>
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVBoxLayout>
#include "CanvasView.h"
#include "ImageTracer.h"
#include "PlotterLink.h"
#include "SvgImport.h"
#include "Transport.h"

static QDoubleSpinBox* dspin(double lo, double hi, double v, double step = 1, int dec = 1) {
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setDecimals(dec);
    s->setSingleStep(step);
    s->setValue(v);
    return s;
}

MainWindow::MainWindow() {
    setWindowTitle("Plotter CoreXY — ESP32");
    resize(1250, 780);
    link_ = new PlotterLink(this);
    canvas_ = new CanvasView;

    auto* left = new QWidget;
    auto* lv = new QVBoxLayout(left);
    lv->addWidget(buildConnectionPanel());
    lv->addWidget(buildControlPanel());
    lv->addStretch();
    left->setMaximumWidth(280);

    auto* tabs = new QTabWidget;
    tabs->addTab(buildJobTab(), "Gambar / Plot");
    tabs->addTab(buildTextTab(), "Teks");
    tabs->addTab(buildCalibTab(), "Kalibrasi");
    tabs->addTab(buildSettingsTab(), "Setelan");
    tabs->addTab(buildFlashTab(), "Flash");
    tabs->addTab(buildTerminalTab(), "Terminal");

    auto* split = new QSplitter;
    split->addWidget(left);
    split->addWidget(canvas_);
    split->addWidget(tabs);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 2);
    setCentralWidget(split);

    connect(link_, &PlotterLink::log, this, [this](const QString& l) { log_->appendPlainText(l); });
    connect(link_, &PlotterLink::reply, this, &MainWindow::onReply);
    connect(link_, &PlotterLink::connectedChanged, this, [this](bool on) {
        connectBtn_->setText(on ? "Putus" : "Hubungkan");
        statusLbl_->setText(on ? "Terhubung" : "Terputus");
        if (on) {
            link_->send("VER");
            loadSettings();
            refreshFiles();
        } else {
            canvas_->setGantry(0, 0, false, false);
        }
    });
    connect(link_, &PlotterLink::status, this, [this](double x, double y, bool pen, bool busy, int q, bool job) {
        canvas_->setGantry(x, y, pen, true);
        curX_ = x; curY_ = y; haveStatus_ = true; curBusy_ = busy || q > 0;
        if (posLbl_) posLbl_->setText(QString("Posisi sekarang:  X %1   Y %2 mm").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2));
        statusLbl_->setText(QString("X %1  Y %2  %3  %4").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2).arg(pen ? "pena turun" : "pena naik").arg(busy || q ? QString("sibuk (q=%1)").arg(q) : "diam"));
        Q_UNUSED(job);
    });
    connect(link_, &PlotterLink::asyncMessage, this, [this](const QString& m) {
        statusBar()->showMessage(m, 6000);
        if (m.startsWith("kalibrasi:") || m.startsWith("home selesai")) loadSettings();   // area hasil ukur
    });
    connect(link_, &PlotterLink::jobProgress, this, [this](int d, int t) {
        progress_->setMaximum(std::max(1, t));
        progress_->setValue(d);
        if (!pendingUpload_.isEmpty()) return;
        canvas_->setProgress(t ? double(d) / t : 0);
    });
    connect(link_, &PlotterLink::jobFinished, this, [this](bool ok, const QString& msg) {
        plotBtn_->setEnabled(true);
        statusBar()->showMessage(ok ? "Selesai: " + msg : "Berhenti: " + msg, 8000);
        if (!pendingUpload_.isEmpty()) { pendingUpload_.clear(); refreshFiles(); }
        if (counterPending_ && ok && counter_) counter_->setValue(counter_->value() + 1);   // penomoran otomatis ({n})
        counterPending_ = false;
    });
    connect(link_, &PlotterLink::limits, this, [this](bool xl, bool yb, bool xr, bool yt) {
        if (!limLbl_) return;
        auto s = [](bool on) { return on ? QString("TERTEKAN") : QString("bebas"); };
        limLbl_->setText(QString("Kiri: %1   Kanan: %2\nBawah: %3   Atas: %4").arg(s(xl), s(xr), s(yb), s(yt)));
    });
    refreshPorts();
    QSettings st;
    host_->setText(st.value("host", "192.168.4.1").toString());
    mode_->setCurrentIndex(st.value("mode", 0).toInt());
    replan();
}

QWidget* MainWindow::buildConnectionPanel() {
    auto* g = new QGroupBox("Koneksi");
    auto* f = new QFormLayout(g);
    mode_ = new QComboBox;
    mode_->addItems({"USB Serial", "WiFi (TCP)"});
    port_ = new QComboBox;
    baud_ = new QComboBox;
    baud_->addItems({"115200", "230400", "460800", "921600"});
    auto* refresh = new QPushButton("Segarkan");
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    host_ = new QLineEdit("192.168.4.1");
    tcpPort_ = new QSpinBox;
    tcpPort_->setRange(1, 65535);
    tcpPort_->setValue(5000);
    connectBtn_ = new QPushButton("Hubungkan");
    connect(connectBtn_, &QPushButton::clicked, this, &MainWindow::toggleConnect);
    statusLbl_ = new QLabel("Terputus");
    statusLbl_->setWordWrap(true);
    f->addRow("Mode", mode_);
    f->addRow("Port COM", port_);
    f->addRow("Baud", baud_);
    f->addRow("", refresh);
    f->addRow("Host", host_);
    f->addRow("TCP", tcpPort_);
    f->addRow(connectBtn_);
    f->addRow(statusLbl_);
    return g;
}

QWidget* MainWindow::buildControlPanel() {
    auto* g = new QGroupBox("Kontrol");
    auto* v = new QVBoxLayout(g);
    step_ = dspin(0.1, 500, 10, 1, 1);
    auto* fl = new QFormLayout;
    fl->addRow("Langkah (mm)", step_);
    v->addLayout(fl);
    auto* sg = new QGroupBox("Kecepatan");
    auto* sf = new QFormLayout(sg);
    travelV_ = dspin(1, 500, 60, 5, 0);
    drawV_ = dspin(0.5, 300, 15, 1, 1);
    travelV_->setSuffix(" mm/s");
    drawV_->setSuffix(" mm/s");
    travelV_->setToolTip("Kecepatan saat pena naik (pindah tanpa menulis) dan jog");
    drawV_->setToolTip("Kecepatan saat pena turun (menulis)");
    sf->addRow("Kosong (tanpa tulis)", travelV_);
    sf->addRow("Menulis", drawV_);
    v->addWidget(sg);
    speedTimer_ = new QTimer(this);
    speedTimer_->setSingleShot(true);
    connect(speedTimer_, &QTimer::timeout, this, [this] {
        link_->send(QString("vmax %1").arg(travelV_->value()));
        link_->send(QString("drawv %1").arg(drawV_->value()));
        cfg_["vmax"] = travelV_->value();
        cfg_["drawv"] = drawV_->value();
    });
    connect(travelV_, &QDoubleSpinBox::valueChanged, this, [this] { speedTimer_->start(500); replan(); });
    connect(drawV_, &QDoubleSpinBox::valueChanged, this, [this] { speedTimer_->start(500); replan(); });
    auto* grid = new QGridLayout;
    auto btn = [&](const QString& t, const QString& d, int r, int c) {
        auto* b = new QPushButton(t);
        connect(b, &QPushButton::clicked, this, [this, d] { jog(d); });
        grid->addWidget(b, r, c);
    };
    btn("▲", "U", 0, 1);
    btn("◀", "L", 1, 0);
    btn("▶", "R", 1, 2);
    btn("▼", "D", 2, 1);
    v->addLayout(grid);
    auto mk = [&](const QString& t, const QString& cmd) {
        auto* b = new QPushButton(t);
        connect(b, &QPushButton::clicked, this, [this, cmd] { link_->send(cmd); });
        v->addWidget(b);
        return b;
    };
    mk("HOME", "HOME");
    mk("Pena naik", "PEN UP");
    mk("Pena turun", "PEN DOWN");
    auto* stop = new QPushButton("STOP");
    stop->setStyleSheet("background:#c0392b;color:white;font-weight:bold;padding:8px");
    connect(stop, &QPushButton::clicked, this, [this] { link_->stop(); });
    v->addWidget(stop);
    v->addWidget(saveBar());
    return g;
}

QWidget* MainWindow::buildJobTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* open = new QPushButton("Buka SVG / Gambar…");
    connect(open, &QPushButton::clicked, this, &MainWindow::openFile);
    saveSvgBtn_ = new QPushButton("Simpan sebagai SVG…");
    connect(saveSvgBtn_, &QPushButton::clicked, this, &MainWindow::saveSvg);
    srcInfo_ = new QLabel("Belum ada file");
    srcInfo_->setWordWrap(true);
    v->addWidget(open);
    v->addWidget(saveSvgBtn_);
    v->addWidget(srcInfo_);

    auto* tg = new QGroupBox("Konversi gambar → garis (raster)");
    auto* tf = new QFormLayout(tg);
    thr_ = new QSlider(Qt::Horizontal);
    thr_->setRange(10, 245);
    thr_->setValue(128);
    invert_ = new QCheckBox("Balik (terang = garis)");
    tf->addRow("Ambang gelap", thr_);
    tf->addRow(invert_);
    connect(thr_, &QSlider::sliderReleased, this, &MainWindow::retrace);
    connect(thr_, &QSlider::valueChanged, this, [this](int) { if (!thr_->isSliderDown()) retrace(); });
    connect(invert_, &QCheckBox::toggled, this, [this] { retrace(); });
    v->addWidget(tg);

    auto* pg = new QGroupBox("Penempatan");
    auto* pf = new QFormLayout(pg);
    areaW_ = dspin(1, 5000, 100, 5, 1);
    areaH_ = dspin(1, 5000, 100, 5, 1);
    margin_ = dspin(0, 200, 5, 1, 1);
    scale_ = dspin(5, 400, 100, 5, 0);
    offX_ = dspin(-2000, 2000, 0, 1, 1);
    offY_ = dspin(-2000, 2000, 0, 1, 1);
    flip_ = new QCheckBox("Balik sumbu Y (SVG ke bawah → mesin ke atas)");
    flip_->setChecked(true);
    optimize_ = new QCheckBox("Urutkan jalur (kurangi perjalanan pena)");
    optimize_->setChecked(true);
    pf->addRow("Area lebar (mm)", areaW_);
    pf->addRow("Area tinggi (mm)", areaH_);
    pf->addRow("Margin (mm)", margin_);
    pf->addRow("Skala (%)", scale_);
    pf->addRow("Geser X (mm)", offX_);
    pf->addRow("Geser Y (mm)", offY_);
    flip_->setText("Konversi Y SVG (jangan diubah kecuali hasil terbalik)");
    mirrorX_ = new QCheckBox("Cermin kiri ↔ kanan");
    mirrorY_ = new QCheckBox("Cermin atas ↔ bawah");
    mirrorX_->setToolTip("Dipakai kalau gambar di kertas tampak terbalik kiri-kanan");
    mirrorY_->setToolTip("Dipakai kalau gambar di kertas tampak terbalik atas-bawah");
    pf->addRow(mirrorX_);
    pf->addRow(mirrorY_);
    pf->addRow(flip_);
    pf->addRow(optimize_);
    auto* sendArea = new QPushButton("Kirim area ke plotter (AREA)");
    connect(sendArea, &QPushButton::clicked, this, [this] {
        link_->send(QString("AREA %1 %2").arg(areaW_->value()).arg(areaH_->value()));
    });
    pf->addRow(sendArea);
    for (auto* s : {areaW_, areaH_, margin_, scale_, offX_, offY_}) connect(s, &QDoubleSpinBox::valueChanged, this, [this] { replan(); });
    connect(flip_, &QCheckBox::toggled, this, [this] { replan(); });
    connect(mirrorX_, &QCheckBox::toggled, this, [this] { replan(); });
    connect(mirrorY_, &QCheckBox::toggled, this, [this] { replan(); });
    connect(optimize_, &QCheckBox::toggled, this, [this] { replan(); });
    v->addWidget(pg);

    planInfo_ = new QLabel;
    planInfo_->setWordWrap(true);
    v->addWidget(planInfo_);
    progress_ = new QProgressBar;
    v->addWidget(progress_);
    plotBtn_ = new QPushButton("▶ Plot sekarang (stream)");
    plotBtn_->setStyleSheet("font-weight:bold;padding:8px");
    connect(plotBtn_, &QPushButton::clicked, this, &MainWindow::plot);
    v->addWidget(plotBtn_);
    auto* ug = new QGroupBox("Simpan job ke flash ESP32");
    auto* uh = new QHBoxLayout(ug);
    jobName_ = new QLineEdit("gambar.job");
    auto* up = new QPushButton("Unggah");
    connect(up, &QPushButton::clicked, this, &MainWindow::uploadJob);
    uh->addWidget(jobName_);
    uh->addWidget(up);
    v->addWidget(ug);
    v->addStretch();
    return w;
}

QWidget* MainWindow::buildTextTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    textTimer_ = new QTimer(this);
    textTimer_->setSingleShot(true);
    textTimer_->setInterval(200);
    connect(textTimer_, &QTimer::timeout, this, &MainWindow::applyText);
    auto changed = [this] { textTimer_->start(); };

    auto* hint = new QLabel("Ketik teks (boleh beberapa baris). Variabel: {n} {n:4} nomor, {tanggal}, {jam}");
    hint->setWordWrap(true);
    v->addWidget(hint);
    textEdit_ = new QPlainTextEdit;
    textEdit_->setPlainText("Halo");
    textEdit_->setMaximumHeight(90);
    connect(textEdit_, &QPlainTextEdit::textChanged, this, changed);
    v->addWidget(textEdit_);

    auto* fg = new QGroupBox("Huruf");
    auto* ff = new QFormLayout(fg);
    textKind_ = new QComboBox;
    textKind_->addItems({"Satu goresan (cocok untuk pena)", "Font Windows (kontur, bisa diarsir)"});
    strokeFont_ = new QComboBox;
    for (const auto& i : strokeFonts()) strokeFont_->addItem(i.label, i.id);
    sysFont_ = new QFontComboBox;
    sysFont_->setCurrentFont(QFont("Arial"));
    textBold_ = new QCheckBox("Tebal");
    textItalic_ = new QCheckBox("Miring");
    auto* st = new QHBoxLayout;
    st->addWidget(textBold_);
    st->addWidget(textItalic_);
    passes_ = new QSpinBox;
    passes_->setRange(1, 3);
    passes_->setToolTip("Gambar huruf 1-3 kali bergeser sedikit supaya garis lebih tebal (font satu goresan)");
    penW_ = dspin(0.1, 2, 0.4, 0.1, 1);
    ff->addRow("Jenis", textKind_);
    ff->addRow("Font goresan", strokeFont_);
    ff->addRow("Font Windows", sysFont_);
    ff->addRow(st);
    ff->addRow("Lintasan (tebal)", passes_);
    ff->addRow("Geser lintasan (mm)", penW_);
    v->addWidget(fg);

    auto* ag = new QGroupBox("Ukuran dan tata letak");
    auto* af = new QFormLayout(ag);
    textH_ = dspin(1, 500, 10, 1, 1);
    textLs_ = dspin(-5, 50, 0, 0.5, 1);
    textLine_ = dspin(0.8, 4, 1.5, 0.1, 2);
    textAlign_ = new QComboBox;
    textAlign_->addItems({"Kiri", "Tengah", "Kanan"});
    textRot_ = dspin(-360, 360, 0, 5, 1);
    af->addRow("Tinggi huruf kapital (mm)", textH_);
    af->addRow("Jarak antar huruf (mm)", textLs_);
    af->addRow("Jarak antar baris (x tinggi)", textLine_);
    af->addRow("Rata", textAlign_);
    af->addRow("Putar (derajat, + = lawan jarum jam)", textRot_);
    v->addWidget(ag);

    auto* hg = new QGroupBox("Isi huruf (font Windows)");
    auto* hf = new QFormLayout(hg);
    hatch_ = new QCheckBox("Isi dengan arsiran");
    hatchSp_ = dspin(0.2, 5, 0.6, 0.1, 2);
    hatchAng_ = dspin(0, 180, 45, 5, 0);
    hf->addRow(hatch_);
    hf->addRow("Jarak arsiran (mm)", hatchSp_);
    hf->addRow("Sudut arsiran", hatchAng_);
    v->addWidget(hg);

    auto* ng = new QGroupBox("Penomoran otomatis");
    auto* nf = new QFormLayout(ng);
    counter_ = new QSpinBox;
    counter_->setRange(0, 999999);
    counter_->setValue(1);
    autoInc_ = new QCheckBox("Naikkan nomor setelah plot selesai");
    nf->addRow("Nomor {n} sekarang", counter_);
    nf->addRow(autoInc_);
    v->addWidget(ng);

    for (auto* s : {textH_, textLs_, textLine_, textRot_, hatchSp_, hatchAng_, penW_}) connect(s, &QDoubleSpinBox::valueChanged, this, changed);
    for (auto* s : {passes_, counter_}) connect(s, &QSpinBox::valueChanged, this, changed);
    for (auto* c : {textBold_, textItalic_, hatch_}) connect(c, &QCheckBox::toggled, this, changed);
    for (auto* c : {textKind_, strokeFont_, textAlign_}) connect(c, &QComboBox::currentIndexChanged, this, changed);
    connect(sysFont_, &QFontComboBox::currentFontChanged, this, changed);

    textInfo_ = new QLabel("Penempatan (margin, geser, mirror) ada di tab Gambar / Plot.");
    textInfo_->setWordWrap(true);
    v->addWidget(textInfo_);
    auto* h = new QHBoxLayout;
    auto* show = new QPushButton("Tampilkan di kanvas");
    auto* go = new QPushButton("Plot teks");
    go->setStyleSheet("font-weight:bold;padding:8px");
    connect(show, &QPushButton::clicked, this, &MainWindow::applyText);
    connect(go, &QPushButton::clicked, this, [this] { applyText(); plot(); counterPending_ = autoInc_->isChecked() && link_->jobActive(); });
    h->addWidget(show);
    h->addWidget(go);
    v->addLayout(h);
    v->addStretch();
    return w;
}

void MainWindow::applyText() {
    TextOptions o;
    o.text = expandTextVars(textEdit_->toPlainText(), counter_->value(), QDateTime::currentDateTime());
    o.singleStroke = textKind_->currentIndex() == 0;
    o.strokeFont = strokeFont_->currentData().toString();
    o.family = sysFont_->currentFont().family();
    o.bold = textBold_->isChecked();
    o.italic = textItalic_->isChecked();
    o.strokePasses = passes_->value();
    o.penWidth = penW_->value();
    o.height = textH_->value();
    o.letterSpacing = textLs_->value();
    o.lineSpacing = textLine_->value();
    o.align = textAlign_->currentIndex();
    o.rotation = textRot_->value();
    o.hatch = hatch_->isChecked();
    o.hatchSpacing = hatchSp_->value();
    o.hatchAngle = hatchAng_->value();
    strokeFont_->setEnabled(o.singleStroke);
    passes_->setEnabled(o.singleStroke);
    penW_->setEnabled(o.singleStroke);
    sysFont_->setEnabled(!o.singleStroke);
    textBold_->setEnabled(!o.singleStroke);
    textItalic_->setEnabled(!o.singleStroke);
    hatch_->setEnabled(!o.singleStroke);
    hatchSp_->setEnabled(!o.singleStroke);
    hatchAng_->setEnabled(!o.singleStroke);
    QString err;
    const Paths p = renderText(o, &err);
    fromRaster_ = false;
    raster_ = QImage();
    if (p.isEmpty()) {
        src_.clear();
        srcBounds_ = QRectF();
        keepSize_ = false;
        replan();
        textInfo_->setText(err.isEmpty() ? "Teks kosong." : err);
        return;
    }
    applySource(p, QString("Teks: %1").arg(o.text.left(40).replace('\n', ' ')), true);
}

QWidget* MainWindow::buildSettingsTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    auto* h = new QHBoxLayout;
    auto mk = [&](const QString& t, const QString& cmd) {
        auto* b = new QPushButton(t);
        if (cmd.isEmpty()) connect(b, &QPushButton::clicked, this, &MainWindow::loadSettings);
        else connect(b, &QPushButton::clicked, this, [this, cmd] { link_->send(cmd); });
        h->addWidget(b);
    };
    mk("Muat", "");
    mk("SAVE (flash)", "SAVE");
    mk("APPLY pin", "APPLY");
    auto* clr = new QPushButton("CLEAR");
    connect(clr, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "CLEAR", "Kembalikan semua setelan ke bawaan?") == QMessageBox::Yes) { link_->send("CLEAR"); loadSettings(); }
    });
    h->addWidget(clr);
    v->addLayout(h);
    settings_ = new QTableWidget(0, 4);
    settings_->setHorizontalHeaderLabels({"Nama", "Nilai", "Rentang", "Keterangan"});
    settings_->horizontalHeader()->setStretchLastSection(true);
    settings_->verticalHeader()->hide();
    settings_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
    connect(settings_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (loadingSettings_ || it->column() != 1) return;
        link_->send(QString("%1 %2").arg(settings_->item(it->row(), 0)->text(), it->text()));
    });
    v->addWidget(settings_);
    auto* wifi = new QGroupBox("WiFi ESP32");
    auto* wf = new QHBoxLayout(wifi);
    auto* ssid = new QLineEdit;
    ssid->setPlaceholderText("SSID");
    auto* pass = new QLineEdit;
    pass->setPlaceholderText("password");
    auto* set = new QPushButton("Set WiFi");
    connect(set, &QPushButton::clicked, this, [this, ssid, pass] {
        if (!ssid->text().isEmpty()) link_->send(QString("WIFI %1 %2").arg(ssid->text(), pass->text()));
    });
    wf->addWidget(ssid);
    wf->addWidget(pass);
    wf->addWidget(set);
    v->addWidget(wifi);
    v->addWidget(saveBar());
    return w;
}

QWidget* MainWindow::buildTerminalTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(3000);
    log_->setFont(QFont("Consolas", 9));
    cmdLine_ = new QLineEdit;
    cmdLine_->setPlaceholderText("perintah, mis. BOX 20  |  HELP");
    connect(cmdLine_, &QLineEdit::returnPressed, this, [this] {
        if (cmdLine_->text().trimmed().isEmpty()) return;
        link_->send(cmdLine_->text().trimmed());
        cmdLine_->clear();
    });
    v->addWidget(log_);
    v->addWidget(cmdLine_);
    return w;
}

QWidget* MainWindow::buildFlashTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    files_ = new QListWidget;
    fsInfo_ = new QLabel;
    auto* h = new QHBoxLayout;
    auto* ref = new QPushButton("Segarkan");
    auto* run = new QPushButton("Jalankan");
    auto* del = new QPushButton("Hapus");
    connect(ref, &QPushButton::clicked, this, &MainWindow::refreshFiles);
    connect(run, &QPushButton::clicked, this, [this] {
        if (auto* it = files_->currentItem()) link_->send("RUN " + it->data(Qt::UserRole).toString());
    });
    connect(del, &QPushButton::clicked, this, [this] {
        if (auto* it = files_->currentItem()) { link_->send("DEL " + it->data(Qt::UserRole).toString()); refreshFiles(); }
    });
    h->addWidget(ref);
    h->addWidget(run);
    h->addWidget(del);
    v->addWidget(fsInfo_);
    v->addWidget(files_);
    v->addLayout(h);

    // Update firmware lewat WiFi (OTA): kirim esp-plotter.bin ke http://IP/ota
    auto* g = new QGroupBox("Update firmware (OTA lewat WiFi)");
    auto* gv = new QVBoxLayout(g);
    auto* row = new QHBoxLayout;
    auto* ip = new QLineEdit;
    ip->setPlaceholderText("IP ESP32, mis. 192.168.1.50");
    ip->setText(host_->text());
    auto* pick = new QPushButton("Pilih .bin & Update");
    row->addWidget(new QLabel("IP:"));
    row->addWidget(ip, 1);
    row->addWidget(pick);
    auto* bar = new QProgressBar;
    bar->setRange(0, 100);
    auto* note = new QLabel("Pilih esp-plotter.bin (bukan esp-plotter-full.bin). Plotter harus diam. ESP32 restart sendiri; firmware yang gagal jalan dikembalikan otomatis.");
    note->setWordWrap(true);
    gv->addLayout(row);
    gv->addWidget(bar);
    gv->addWidget(note);
    v->addWidget(g);
    connect(pick, &QPushButton::clicked, this, [this, ip, bar, pick] {
        const QString host = ip->text().trimmed();
        if (host.isEmpty()) { QMessageBox::warning(this, "OTA", "Isi IP ESP32 dulu (lihat perintah IP di Terminal)."); return; }
        const QString path = QFileDialog::getOpenFileName(this, "Firmware", QString(), "Firmware (*.bin)");
        if (path.isEmpty()) return;
        auto* f = new QFile(path);
        if (!f->open(QIODevice::ReadOnly)) { delete f; QMessageBox::warning(this, "OTA", "File tidak bisa dibuka."); return; }
        QNetworkRequest rq(QUrl("http://" + host + "/ota"));
        rq.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
        rq.setTransferTimeout(60000);
        auto* nam = new QNetworkAccessManager(this);
        QNetworkReply* rep = nam->post(rq, f);
        f->setParent(rep);
        pick->setEnabled(false);
        bar->setValue(0);
        connect(rep, &QNetworkReply::uploadProgress, this, [bar](qint64 a, qint64 b) { if (b > 0) bar->setValue(int(a * 100 / b)); });
        connect(rep, &QNetworkReply::finished, this, [this, rep, nam, bar, pick] {
            const QByteArray body = rep->readAll().trimmed();
            const bool ok = rep->error() == QNetworkReply::NoError;
            pick->setEnabled(true);
            if (ok) { bar->setValue(100); QMessageBox::information(this, "OTA", "Selesai. ESP32 restart; sambungkan ulang setelah beberapa detik."); }
            else QMessageBox::warning(this, "OTA gagal", rep->errorString() + (body.isEmpty() ? QString() : "\n" + QString::fromUtf8(body)));
            rep->deleteLater();
            nam->deleteLater();
        });
    });
    return w;
}

// ---------------------------------------------------------------- aksi
void MainWindow::refreshPorts() {
    port_->clear();
    port_->addItems(availableSerialPorts());
}

void MainWindow::toggleConnect() {
    if (link_->connected()) { link_->detach(); return; }
    QString err;
    Transport* t = mode_->currentIndex() == 0
        ? makeSerialTransport(port_->currentText(), baud_->currentText().toInt(), this)
        : makeTcpTransport(host_->text().trimmed(), quint16(tcpPort_->value()), this);
    if (mode_->currentIndex() == 0 && port_->currentText().isEmpty()) { delete t; QMessageBox::warning(this, "Koneksi", "Tidak ada port COM."); return; }
    if (!t->open(&err)) {
        delete t;
        QMessageBox::warning(this, "Koneksi gagal", err);
        return;
    }
    QSettings st;
    st.setValue("host", host_->text());
    st.setValue("mode", mode_->currentIndex());
    link_->attach(t);
}

void MainWindow::jog(const QString& dir) { link_->send(QString("%1 %2").arg(dir).arg(step_->value())); }

void MainWindow::openFile() {
    QString f = QFileDialog::getOpenFileName(this, "Buka gambar", QString(), "SVG, G-code & gambar (*.svg *.gcode *.nc *.ngc *.gc *.tap *.png *.jpg *.jpeg *.bmp *.gif);;Semua (*)");
    if (f.isEmpty()) return;
    static const QStringList gExt = {"gcode", "nc", "ngc", "gc", "tap"};
    if (gExt.contains(QFileInfo(f).suffix().toLower())) {
        QFile file(f);
        if (!file.open(QIODevice::ReadOnly)) return;
        GcodeResult r = parseGcode(file.readAll());
        if (!r.error.isEmpty()) { QMessageBox::warning(this, "G-code", r.error); return; }
        fromRaster_ = false;
        raster_ = QImage();
        applySource(r.paths, QString("%1 (G-code, %2 baris)\nGaris %3 mm, perjalanan pena naik %4 mm")
                                 .arg(QFileInfo(f).fileName()).arg(r.lines).arg(r.draw, 0, 'f', 0).arg(r.travel, 0, 'f', 0)
                                 + (r.warning.isEmpty() ? "" : "\n" + r.warning));
    } else if (f.endsWith(".svg", Qt::CaseInsensitive)) {
        QFile file(f);
        if (!file.open(QIODevice::ReadOnly)) return;
        SvgResult r = parseSvg(file.readAll());
        if (!r.error.isEmpty()) { QMessageBox::warning(this, "SVG", r.error); return; }
        fromRaster_ = false;
        raster_ = QImage();
        applySource(r.paths, QFileInfo(f).fileName() + (r.warning.isEmpty() ? "" : "\n" + r.warning));
    } else {
        QImage img(f);
        if (img.isNull()) { QMessageBox::warning(this, "Gambar", "Gagal membaca gambar."); return; }
        raster_ = img;
        fromRaster_ = true;
        retrace();
        srcInfo_->setText(QFileInfo(f).fileName() + "\n" + srcInfo_->text());
    }
}

void MainWindow::retrace() {
    if (!fromRaster_ || raster_.isNull()) return;
    TraceOptions o;
    o.threshold = thr_->value();
    o.invert = invert_->isChecked();
    QSizeF sz;
    Paths p = traceImage(raster_, o, &sz);
    applySource(p, QString("Raster %1x%2 px → %3 kontur").arg(raster_.width()).arg(raster_.height()).arg(p.size()));
}

void MainWindow::applySource(const Paths& p, const QString& info, bool keepSize) {
    keepSize_ = keepSize;
    src_ = p;
    srcBounds_ = boundsOf(p);
    srcInfo_->setText(info + QString("\n%1 jalur, ukuran sumber %2 x %3").arg(p.size()).arg(srcBounds_.width(), 0, 'f', 1).arg(srcBounds_.height(), 0, 'f', 1));
    replan();
}

void MainWindow::replan() {
    canvas_->setArea(areaW_->value(), areaH_->value());
    PlanOptions o;
    o.areaW = areaW_->value();
    o.areaH = areaH_->value();
    o.margin = margin_->value();
    o.scalePct = scale_->value();
    o.offsetX = offX_->value();
    o.offsetY = offY_->value();
    o.flipY = flip_->isChecked();
    o.mirrorX = mirrorX_ && mirrorX_->isChecked();
    o.mirrorY = mirrorY_ && mirrorY_->isChecked();
    o.optimize = optimize_->isChecked();
    o.keepSize = keepSize_;
    plan_ = planPaths(src_, o);
    const double dv = drawV_ ? drawV_->value() : 15.0, tv = travelV_ ? travelV_->value() : 60.0;
    canvas_->setPaths(plan_.paths);
    planInfo_->setText(plan_.paths.isEmpty() ? "Belum ada jalur."
                                             : QString("%1 jalur, %2 titik. Garis %3 mm, pindah %4 mm. Perkiraan %5 menit pada %6 mm/s.")
                                                   .arg(plan_.paths.size()).arg(plan_.points).arg(plan_.drawLength, 0, 'f', 0)
                                                   .arg(plan_.travelLength, 0, 'f', 0)
                                                   .arg(plan_.drawLength / dv / 60 + plan_.travelLength / tv / 60, 0, 'f', 1).arg(dv, 0, 'f', 0));

    if (keepSize_ && !plan_.paths.isEmpty()) {
        const double sc = scale_->value() / 100.0;
        const bool tooBig = srcBounds_.width() * sc > areaW_->value() - 2 * margin_->value() + 0.01 || srcBounds_.height() * sc > areaH_->value() - 2 * margin_->value() + 0.01;
        const QString t = QString("Ukuran teks %1 x %2 mm").arg(srcBounds_.width() * sc, 0, 'f', 1).arg(srcBounds_.height() * sc, 0, 'f', 1) +
                          (tooBig ? " - MELEBIHI area kerja, bagian tepi terpotong. Kecilkan huruf." : "");
        planInfo_->setText(planInfo_->text() + "\n" + t);
        if (textInfo_) textInfo_->setText(t);
    } else if (textInfo_) textInfo_->setText(planInfo_->text());
}

QStringList MainWindow::jobCommands() const { return toCommands(plan_); }

void MainWindow::saveSvg() {
    if (src_.isEmpty()) { QMessageBox::information(this, "SVG", "Belum ada jalur."); return; }
    QString f = QFileDialog::getSaveFileName(this, "Simpan SVG", "gambar.svg", "SVG (*.svg)");
    if (f.isEmpty()) return;
    QFile file(f);
    if (!file.open(QIODevice::WriteOnly)) { QMessageBox::warning(this, "SVG", "Gagal menulis file."); return; }
    file.write(pathsToSvg(src_, srcBounds_).toUtf8());
    statusBar()->showMessage("Tersimpan: " + f, 5000);
}

void MainWindow::plot() {
    if (!link_->connected()) { QMessageBox::information(this, "Plot", "Hubungkan plotter dulu."); return; }
    if (plan_.paths.isEmpty()) { QMessageBox::information(this, "Plot", "Belum ada jalur."); return; }
    if (link_->jobActive()) return;
    if (QMessageBox::question(this, "Plot", "Mulai plot? Pastikan sudah HOME dan kertas terpasang.") != QMessageBox::Yes) return;
    pendingUpload_.clear();
    plotBtn_->setEnabled(false);
    link_->startJob(jobCommands(), true);
}

void MainWindow::uploadJob() {
    if (!link_->connected() || plan_.paths.isEmpty() || link_->jobActive()) { statusBar()->showMessage("Hubungkan plotter dan siapkan jalur dulu", 4000); return; }
    QString name = jobName_->text().trimmed();
    if (name.isEmpty() || name.contains('/')) return;
    QStringList lines;
    lines << "PUTF " + name;
    lines << jobCommands();
    lines << "ENDF";
    pendingUpload_ = lines;
    link_->startJob(lines, false);
}

void MainWindow::loadSettings() {
    if (!link_->connected()) return;
    link_->send("LIST");
}

void MainWindow::refreshFiles() {
    if (!link_->connected()) return;
    link_->send("LS");
    link_->send("FSINFO");
}

void MainWindow::onReply(const QString& cmd, const QStringList& data, const QString& status) {
    if (cmd == "LIST") {
        loadingSettings_ = true;
        settings_->setRowCount(0);
        for (const QString& d : data) {
            if (!d.startsWith("S ")) continue;
            QStringList p = d.split(' ');
            if (p.size() < 5) continue;
            int r = settings_->rowCount();
            settings_->insertRow(r);
            auto* n = new QTableWidgetItem(p[1]);
            n->setFlags(n->flags() & ~Qt::ItemIsEditable);
            settings_->setItem(r, 0, n);
            settings_->setItem(r, 1, new QTableWidgetItem(p[2]));
            auto* rg = new QTableWidgetItem(p[3] + " … " + p[4]);
            rg->setFlags(rg->flags() & ~Qt::ItemIsEditable);
            settings_->setItem(r, 2, rg);
            auto* ds = new QTableWidgetItem(p.mid(5).join(' '));
            ds->setFlags(ds->flags() & ~Qt::ItemIsEditable);
            settings_->setItem(r, 3, ds);
            cfg_[p[1]] = p[2].toDouble();
            if (p[1] == "areaW") areaW_->setValue(p[2].toDouble());
            if (p[1] == "areaH") areaH_->setValue(p[2].toDouble());
        }
        settings_->resizeColumnToContents(0);
        loadingSettings_ = false;
        syncCalib();
    } else if (cmd == "LIM") {
        for (const QString& d : data)
            if (d.startsWith("LIM") && limLbl_) limLbl_->setText("Saklar: " + d.mid(4) + (d.contains("=1") ? "  (ada yang tertekan)" : "  (bebas)"));
    } else if (cmd == "LS") {
        files_->clear();
        for (const QString& d : data) {
            if (!d.startsWith("F ")) continue;
            QStringList p = d.split(' ');
            auto* it = new QListWidgetItem(QString("%1   (%2 B)").arg(p.value(1), p.value(2)));
            it->setData(Qt::UserRole, p.value(1));
            files_->addItem(it);
        }
    } else if (cmd == "FSINFO") {
        for (const QString& d : data) if (d.startsWith("FS ")) fsInfo_->setText("Flash: " + d.mid(3));
    }
    if (status.startsWith("error") || status == "busy" || status == "full") statusBar()->showMessage(cmd + " → " + status, 5000);
}

// ---------------------------------------------------------------- kalibrasi
QWidget* MainWindow::saveBar() {
    auto* b = new QPushButton("SAVE ke flash ESP32");
    b->setStyleSheet("background:#1e7e34;color:white;font-weight:bold;padding:7px");
    b->setToolTip("Simpan semua setelan (servo, homing, langkah/mm, pin, dll) agar tetap ada setelah dimatikan");
    connect(b, &QPushButton::clicked, this, [this] {
        if (!link_->connected()) { statusBar()->showMessage("Hubungkan plotter dulu", 3000); return; }
        link_->send("SAVE");
        statusBar()->showMessage("Setelan disimpan ke flash", 4000);
    });
    return b;
}

void MainWindow::syncCalib() {
    if (!svSlider_) return;
    svUpLbl_->setText(QString("Pena naik = %1°").arg(cfg("svUp", 0), 0, 'f', 0));
    svDnLbl_->setText(QString("Pena turun = %1°").arg(cfg("svDn", 90), 0, 'f', 0));
    svMinUs_->setValue(int(cfg("svMinUs", 500)));
    svMaxUs_->setValue(int(cfg("svMaxUs", 2500)));
    penMs_->setValue(int(cfg("penMs", 300)));
    hFast_->setValue(cfg("hFast", 30));
    hOff_->setValue(cfg("hOff", 10));
    hSearch_->setValue(cfg("hSearch", 400));
    hdx_->setCurrentIndex(cfg("hdx", -1) < 0 ? 0 : 1);
    hdy_->setCurrentIndex(cfg("hdy", -1) < 0 ? 0 : 1);
    hCenter_->setChecked(cfg("hCenter", 1) != 0);
    QSignalBlocker ba(invA_), bb(invB_);
    invA_->setChecked(cfg("invA", 0) != 0);
    invB_->setChecked(cfg("invB", 0) != 0);
    spmmLbl_->setText(QString("Langkah per mm saat ini: %1").arg(cfg("spmm", 80)));
    QSignalBlocker bt(travelV_), bd(drawV_);
    travelV_->setValue(cfg("vmax", 60));
    drawV_->setValue(cfg("drawv", 15));
}

QWidget* MainWindow::buildCalibTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->addWidget(saveBar());  // SAVE atas

    // ---- kalibrasi manual: jog ke tiap tepi, tandai, terapkan
    auto* mg0 = new QGroupBox("0. Kalibrasi MANUAL (jog ke tepi, lalu tandai)");
    auto* mv0 = new QVBoxLayout(mg0);
    auto* help0 = new QLabel("Gerakkan gantry dengan tombol panah (panel kiri) sampai pena tepat di tiap tepi area, lalu klik tombol tepi itu. "
                             "Setelah keempatnya ditandai, klik Terapkan: titik nol (0,0) dipindah ke sudut kiri-bawah dan ukuran area dihitung otomatis.");
    help0->setWordWrap(true);
    mv0->addWidget(help0);
    posLbl_ = new QLabel("Posisi sekarang: -");
    posLbl_->setStyleSheet("font-weight:bold");
    mv0->addWidget(posLbl_);
    static const char* const edgeName[4] = {"KIRI", "KANAN", "BAWAH", "ATAS"};
    auto* eg = new QGridLayout;
    for (int i = 0; i < 4; i++) {
        auto* b = new QPushButton(QString("Tandai %1").arg(edgeName[i]));
        edgeLbl_[i] = new QLabel("-");
        connect(b, &QPushButton::clicked, this, [this, i] {
            if (!haveStatus_) { statusBar()->showMessage("Belum ada data posisi, hubungkan plotter dulu", 4000); return; }
            edge_[i] = (i < 2) ? curX_ : curY_;                    // kiri/kanan = X, bawah/atas = Y
            edgeSet_[i] = true;
            edgeLbl_[i]->setText(QString("%1 = %2 mm").arg(i < 2 ? "X" : "Y").arg(edge_[i], 0, 'f', 2));
        });
        eg->addWidget(b, i / 2, (i % 2) * 2);
        eg->addWidget(edgeLbl_[i], i / 2, (i % 2) * 2 + 1);
    }
    mv0->addLayout(eg);
    auto* applyMan = new QPushButton("Terapkan: nol di kiri-bawah + hitung ukuran area");
    applyMan->setStyleSheet("font-weight:bold;padding:6px");
    connect(applyMan, &QPushButton::clicked, this, [this] {
        if (!link_->connected()) return;
        for (int i = 0; i < 4; i++)
            if (!edgeSet_[i]) { QMessageBox::information(this, "Kalibrasi manual", "Tandai dulu keempat tepi (kiri, kanan, bawah, atas)."); return; }
        double w = edge_[1] - edge_[0], h = edge_[3] - edge_[2];
        if (w < 1 || h < 1) {
            QMessageBox::warning(this, "Kalibrasi manual", QString("Ukuran tidak masuk akal (lebar %1, tinggi %2 mm). Kanan harus lebih besar dari kiri, atas dari bawah.").arg(w).arg(h));
            return;
        }
        if (curBusy_) { QMessageBox::information(this, "Kalibrasi manual", "Tunggu gantry berhenti dulu."); return; }
        // geser kerangka koordinat: titik kiri-bawah menjadi (0,0); posisi gantry sekarang ikut dihitung ulang
        link_->send(QString("SETPOS %1 %2").arg(curX_ - edge_[0], 0, 'f', 3).arg(curY_ - edge_[2], 0, 'f', 3));
        link_->send(QString("AREA %1 %2").arg(w, 0, 'f', 2).arg(h, 0, 'f', 2));
        cfg_["areaW"] = w; cfg_["areaH"] = h;
        saveCalibrationRecord(w, h);
        areaW_->setValue(w); areaH_->setValue(h);
        manInfo_->setText(QString("Selesai: area %1 x %2 mm, titik nol di kiri-bawah. Klik SAVE untuk menyimpan ukuran area (posisi nol hilang saat dimatikan).").arg(w, 0, 'f', 1).arg(h, 0, 'f', 1));
    });
    mv0->addWidget(applyMan);
    auto* row0 = new QHBoxLayout;
    auto* zeroHere = new QPushButton("Jadikan posisi ini (0,0)");
    connect(zeroHere, &QPushButton::clicked, this, [this] { link_->send("ZERO"); });
    auto* toCenter = new QPushButton("Pindah ke tengah area");
    connect(toCenter, &QPushButton::clicked, this, [this] {
        link_->send("PU");
        link_->send(QString("MV %1 %2").arg(areaW_->value() / 2).arg(areaH_->value() / 2));
    });
    auto* reset = new QPushButton("Hapus tanda");
    connect(reset, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < 4; i++) { edgeSet_[i] = false; edgeLbl_[i]->setText("-"); }
        manInfo_->clear();
    });
    row0->addWidget(zeroHere);
    row0->addWidget(toCenter);
    row0->addWidget(reset);
    mv0->addLayout(row0);
    manInfo_ = new QLabel;
    manInfo_->setWordWrap(true);
    mv0->addWidget(manInfo_);
    v->addWidget(mg0);

    // ---- servo
    auto* sg = new QGroupBox("1. Servo pena");
    auto* sv = new QVBoxLayout(sg);
    svSlider_ = new QSlider(Qt::Horizontal);
    svSlider_->setRange(0, 180);
    svSlider_->setValue(45);
    svAngle_ = new QSpinBox;
    svAngle_->setRange(0, 180);
    svAngle_->setValue(45);
    svAngle_->setSuffix("°");
    auto* row = new QHBoxLayout;
    row->addWidget(svSlider_, 1);
    row->addWidget(svAngle_);
    sv->addLayout(row);
    angleTimer_ = new QTimer(this);
    angleTimer_->setSingleShot(true);
    connect(angleTimer_, &QTimer::timeout, this, [this] { link_->send(QString("ANGLE %1").arg(svAngle_->value())); });
    connect(svSlider_, &QSlider::valueChanged, this, [this](int a) {
        QSignalBlocker b(svAngle_);
        svAngle_->setValue(a);
        angleTimer_->start(120);  // kirim setelah slider berhenti sebentar
    });
    connect(svAngle_, &QSpinBox::valueChanged, this, [this](int a) {
        QSignalBlocker b(svSlider_);
        svSlider_->setValue(a);
        angleTimer_->start(120);
    });
    auto* br = new QHBoxLayout;
    auto* setUp = new QPushButton("Set sebagai pena NAIK");
    auto* setDn = new QPushButton("Set sebagai pena TURUN");
    connect(setUp, &QPushButton::clicked, this, [this] {
        link_->send(QString("svUp %1").arg(svAngle_->value()));
        cfg_["svUp"] = svAngle_->value();
        syncCalib();
    });
    connect(setDn, &QPushButton::clicked, this, [this] {
        link_->send(QString("svDn %1").arg(svAngle_->value()));
        cfg_["svDn"] = svAngle_->value();
        syncCalib();
    });
    br->addWidget(setUp);
    br->addWidget(setDn);
    sv->addLayout(br);
    svUpLbl_ = new QLabel;
    svDnLbl_ = new QLabel;
    sv->addWidget(svUpLbl_);
    sv->addWidget(svDnLbl_);
    auto* tr = new QHBoxLayout;
    auto* tUp = new QPushButton("Tes naik");
    auto* tDn = new QPushButton("Tes turun");
    connect(tUp, &QPushButton::clicked, this, [this] { link_->send("PEN UP"); });
    connect(tDn, &QPushButton::clicked, this, [this] { link_->send("PEN DOWN"); });
    tr->addWidget(tUp);
    tr->addWidget(tDn);
    sv->addLayout(tr);
    auto* pf = new QFormLayout;
    svMinUs_ = new QSpinBox;
    svMinUs_->setRange(100, 3000);
    svMaxUs_ = new QSpinBox;
    svMaxUs_->setRange(500, 3000);
    penMs_ = new QSpinBox;
    penMs_->setRange(0, 3000);
    pf->addRow("Pulsa 0° (µs)", svMinUs_);
    pf->addRow("Pulsa 180° (µs)", svMaxUs_);
    pf->addRow("Jeda pena (ms)", penMs_);
    auto* applyPulse = new QPushButton("Terapkan pulsa & jeda");
    connect(applyPulse, &QPushButton::clicked, this, [this] {
        link_->send(QString("SVUS %1 %2").arg(svMinUs_->value()).arg(svMaxUs_->value()));
        link_->send(QString("penMs %1").arg(penMs_->value()));
        cfg_["svMinUs"] = svMinUs_->value();
        cfg_["svMaxUs"] = svMaxUs_->value();
        cfg_["penMs"] = penMs_->value();
    });
    pf->addRow(applyPulse);
    sv->addLayout(pf);
    v->addWidget(sg);

    // ---- homing
    auto* hg = new QGroupBox("2. Kalibrasi otomatis (HOME)");
    auto* hf = new QFormLayout(hg);
    limLbl_ = new QLabel("Saklar: -");
    hf->addRow(limLbl_);
    auto* calHelp = new QLabel("Kalibrasi area: ke kiri sampai saklar kiri tertekan, lalu langsung berbalik ke kanan sampai saklar kanan "
                               "tertekan, lalu bawah → atas. Lebar & tinggi area terukur otomatis, titik nol di kiri-bawah. "
                               "Butuh 4 saklar (pin kanan = pinLX2, atas = pinLY2).");
    calHelp->setWordWrap(true);
    hf->addRow(calHelp);
    auto* calBtn = new QPushButton("▶ KALIBRASI AREA (kiri-kanan, bawah-atas)");
    calBtn->setStyleSheet("font-weight:bold;padding:8px");
    connect(calBtn, &QPushButton::clicked, this, [this] {
        if (!link_->connected()) return;
        if (QMessageBox::question(this, "Kalibrasi", "Gantry akan bergerak ke kiri, kanan, bawah, lalu atas sampai tiap saklar tertekan. Lanjut?") == QMessageBox::Yes)
            link_->send("CAL");
    });
    hf->addRow(calBtn);
    hdx_ = new QComboBox;
    hdx_->addItems({"X minimum (kiri)", "X maksimum (kanan)"});
    hdy_ = new QComboBox;
    hdy_->addItems({"Y minimum (bawah)", "Y maksimum (atas)"});
    hFast_ = dspin(1, 300, 30, 5, 1);
    hOff_ = dspin(0, 100, 10, 1, 1);
    hSearch_ = dspin(10, 5000, 400, 10, 0);
    hCenter_ = new QCheckBox("Pindah ke tengah area setelah HOME");
    hf->addRow("Saklar X di", hdx_);
    hf->addRow("Saklar Y di", hdy_);
    hf->addRow("Kecepatan (mm/s)", hFast_);
    hf->addRow("Jarak nol dari saklar (mm)", hOff_);
    hf->addRow("Jarak cari maks (mm)", hSearch_);
    hf->addRow(hCenter_);
    auto* hb = new QHBoxLayout;
    auto* applyHome = new QPushButton("Terapkan");
    auto* runHome = new QPushButton("▶ HOME sekarang");
    runHome->setStyleSheet("font-weight:bold");
    connect(applyHome, &QPushButton::clicked, this, [this] {
        link_->send(QString("HDIR %1 %2").arg(hdx_->currentIndex() ? 1 : -1).arg(hdy_->currentIndex() ? 1 : -1));
        link_->send(QString("hFast %1").arg(hFast_->value()));
        link_->send(QString("hOff %1").arg(hOff_->value()));
        link_->send(QString("hSearch %1").arg(hSearch_->value()));
        link_->send(QString("hCenter %1").arg(hCenter_->isChecked() ? 1 : 0));
    });
    connect(runHome, &QPushButton::clicked, this, [this] { link_->send("HOME"); });
    hb->addWidget(applyHome);
    hb->addWidget(runHome);
    hf->addRow(hb);
    v->addWidget(hg);

    // ---- langkah per mm & arah motor
    auto* mg = new QGroupBox("3. Ukuran & arah motor");
    auto* mf = new QFormLayout(mg);
    spmmLbl_ = new QLabel;
    testSize_ = dspin(5, 300, 20, 5, 1);
    measured_ = dspin(0.1, 1000, 20, 0.5, 2);
    invA_ = new QCheckBox("Balik motor A");
    invB_ = new QCheckBox("Balik motor B");
    auto* box = new QPushButton("Gambar kotak uji (pena turun)");
    auto* calc = new QPushButton("Hitung && terapkan langkah/mm");
    mf->addRow(spmmLbl_);
    mf->addRow("Sisi kotak uji (mm)", testSize_);
    mf->addRow(box);
    mf->addRow("Sisi terukur dengan penggaris (mm)", measured_);
    mf->addRow(calc);
    mf->addRow(invA_);
    mf->addRow(invB_);
    connect(box, &QPushButton::clicked, this, [this] { link_->send(QString("BOX %1").arg(testSize_->value())); });
    connect(calc, &QPushButton::clicked, this, [this] {
        double cur = cfg("spmm", 80), m = measured_->value();
        if (m <= 0) return;
        // firmware menggambar sisi = diminta * spmm_lama / spmm_sebenarnya  =>  spmm_sebenarnya = spmm_lama * diminta / terukur
        double ns = cur * testSize_->value() / m;
        link_->send(QString("spmm %1").arg(ns, 0, 'f', 3));
        cfg_["spmm"] = ns;
        syncCalib();
    });
    connect(invA_, &QCheckBox::toggled, this, [this](bool on) { link_->send(QString("invA %1").arg(on ? 1 : 0)); });
    connect(invB_, &QCheckBox::toggled, this, [this](bool on) { link_->send(QString("invB %1").arg(on ? 1 : 0)); });
    v->addWidget(mg);

    v->addStretch();
    v->addWidget(saveBar());  // SAVE bawah
    return w;
}

// Satu baris JSON per kalibrasi manual (JSON Lines), di folder yang sama dengan QtPlotter.exe.
// Berisi tepi yang ditandai, ukuran hasil, dan snapshot setelan ESP32 saat itu, untuk menyetel kalibrasi otomatis.
void MainWindow::saveCalibrationRecord(double w, double h) {
    QJsonObject o;
    o["waktu"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    o["jenis"] = "manual";
    QJsonObject edges;
    edges["kiri_x_mm"] = edge_[0];
    edges["kanan_x_mm"] = edge_[1];
    edges["bawah_y_mm"] = edge_[2];
    edges["atas_y_mm"] = edge_[3];
    o["tepi"] = edges;
    o["posisi_saat_terapkan"] = QJsonObject{{"x_mm", curX_}, {"y_mm", curY_}};
    o["lebar_mm"] = w;
    o["tinggi_mm"] = h;
    o["geser_nol_mm"] = QJsonObject{{"x", curX_ - edge_[0]}, {"y", curY_ - edge_[2]}};
    QJsonObject cfg;
    for (auto it = cfg_.constBegin(); it != cfg_.constEnd(); ++it) cfg[it.key()] = it.value();
    o["setelan_esp32"] = cfg;
    // folder data pengguna (tetap ada walau exe dijalankan dari paket 1 file): %APPDATA%/EmbeddedPlotter/QtPlotter
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString path = dir + "/calibration_data.jsonl";
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
        statusBar()->showMessage("Data kalibrasi tersimpan: " + QDir::toNativeSeparators(path), 8000);
    } else {
        statusBar()->showMessage("Gagal menulis " + path, 8000);
    }
}
