#include "MainWindow.h"
#include "core/MapDeployer.h"

#include "protocol/RbkProtocol.h"

#include <QAction>
#include <QAbstractSocket>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStringList>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <cmath>
#include <limits>

namespace {
constexpr int MaxConcurrentRobots = 3;
constexpr int MaxVisibleSamples = 2000;
constexpr double LowConfidenceThreshold = 0.60;
constexpr double ConfidenceDropThreshold = 0.20;
constexpr double PositionJumpSpeed = 3.0;
constexpr double PositionJumpDistance = 0.50;

struct HeatmapImportResult {
    QVector<ConfidenceHeatmapCell> cells;
    QString robotName;
    QString error;
    int rows = 0;
    int movingSamples = 0;
    int rejectedSamples = 0;
    int files = 0;
};

QString statusName(int status, bool english)
{
    if (status < 0) return english ? QStringLiteral("Unknown") : QStringLiteral("未知");
    switch (status) {
    case 0: return english ? QStringLiteral("Idle") : QStringLiteral("空闲");
    case 1: return english ? QStringLiteral("Waiting") : QStringLiteral("等待");
    case 2: return english ? QStringLiteral("Running") : QStringLiteral("运行中");
    case 3: return english ? QStringLiteral("Suspended") : QStringLiteral("已暂停");
    case 4: return english ? QStringLiteral("Completed") : QStringLiteral("已完成");
    case 5: return english ? QStringLiteral("Failed") : QStringLiteral("失败");
    case 6: return english ? QStringLiteral("Canceled") : QStringLiteral("已取消");
    default: return (english ? QStringLiteral("Unknown (%1)") : QStringLiteral("未知(%1)"))
                        .arg(status);
    }
}

QString knownResultText(const QString &text, bool english)
{
    static const QList<QPair<QString, QString>> values = {
        {QStringLiteral("按 MD5 查找地图…"), QStringLiteral("Finding the uploaded map by MD5...")},
        {QStringLiteral("已跳过：机器人忙"), QStringLiteral("Skipped: robot is busy")},
        {QStringLiteral("当前地图 MD5 已匹配"), QStringLiteral("Current map already matches the requested MD5")},
        {QStringLiteral("等待操作"), QStringLiteral("Waiting")},
        {QStringLiteral("查询地图状态…"), QStringLiteral("Querying map status...")},
        {QStringLiteral("状态已刷新"), QStringLiteral("Status refreshed")},
        {QStringLiteral("下载当前地图…"), QStringLiteral("Downloading current map...")},
        {QStringLiteral("等待预检"), QStringLiteral("Waiting for precheck")},
        {QStringLiteral("检查导航状态…"), QStringLiteral("Checking navigation status...")},
        {QStringLiteral("上传地图…"), QStringLiteral("Uploading map...")},
        {QStringLiteral("校验机器人 MD5…"), QStringLiteral("Verifying robot MD5...")},
        {QStringLiteral("切换地图…"), QStringLiteral("Switching map...")},
        {QStringLiteral("验证当前地图…"), QStringLiteral("Verifying current map...")},
        {QStringLiteral("上传及 MD5 校验成功"), QStringLiteral("Upload and MD5 verification succeeded")},
        {QStringLiteral("上传、校验和切换成功"), QStringLiteral("Upload, verification and switch succeeded")},
        {QStringLiteral("当前地图验证不一致"), QStringLiteral("Current map verification mismatch")},
    };
    for (const auto &value : values) {
        if (text == value.first || text == value.second) return english ? value.second : value.first;
    }
    return text;
}

QString normalizedMapName(QString name)
{
    name = name.trimmed();
    while (name.endsWith(QStringLiteral(".smap"), Qt::CaseInsensitive)) {
        name.chop(5);
        name = name.trimmed();
    }
    return name.toCaseFolded();
}

QString localizationMethodName(int method, bool english)
{
    switch (method) {
    case 0: return english ? QStringLiteral("Natural features") : QStringLiteral("自然轮廓");
    case 1: return english ? QStringLiteral("Reflectors") : QStringLiteral("反光柱");
    case 2: return english ? QStringLiteral("QR code") : QStringLiteral("二维码");
    case 3: return english ? QStringLiteral("Odometry") : QStringLiteral("里程计");
    case 4: return english ? QStringLiteral("3D localization") : QStringLiteral("3D 定位");
    case 5: return english ? QStringLiteral("Ceiling code") : QStringLiteral("天码");
    case 6: return english ? QStringLiteral("Feature localization") : QStringLiteral("特征定位");
    case 7: return english ? QStringLiteral("3D feature localization") : QStringLiteral("3D 特征定位");
    case 8: return english ? QStringLiteral("3D KF localization") : QStringLiteral("3D KF 定位");
    default: return english ? QStringLiteral("Unknown") : QStringLiteral("未知");
    }
}

QByteArray compactJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    m_english = QSettings().value(QStringLiteral("language"), QStringLiteral("zh"))
                    .toString() == QStringLiteral("en");
    buildUi();
    loadRobots();
}

void MainWindow::buildUi()
{
    resize(1180, 720);

    m_toolbar = addToolBar(QString());
    m_toolbar->setMovable(false);
    m_logoLabel = new QLabel(m_toolbar);
    m_logoLabel->setFixedSize(58, 44);
    m_logoLabel->setContentsMargins(4, 2, 8, 2);
    m_logoLabel->setPixmap(QPixmap(QStringLiteral(":/branding/svi-logo-128.png"))
                               .scaled(46, 38, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_toolbar->addWidget(m_logoLabel);
    setWindowIcon(QIcon(QStringLiteral(":/branding/svi-logo-128.png")));
    m_addAction = m_toolbar->addAction(QString(), this, &MainWindow::addRobot);
    m_removeAction = m_toolbar->addAction(QString(), this, &MainWindow::removeSelected);
    m_toolbar->addSeparator();
    m_refreshAction = m_toolbar->addAction(QString(), this, &MainWindow::refreshSelected);
    m_downloadAction = m_toolbar->addAction(QString(), this, &MainWindow::downloadMap);
    m_openMapAction = m_toolbar->addAction(QString(), this, &MainWindow::openMapFile);
    m_showMapAction = m_toolbar->addAction(QString(), this, [this] { showMapWindow(); });
    m_livePositionAction = m_toolbar->addAction(QString());
    m_livePositionAction->setCheckable(true);
    connect(m_livePositionAction, &QAction::toggled, this, [this](bool enabled) {
        if (enabled) startLivePosition();
        else stopLivePosition();
    });
    m_clearTrackAction = m_toolbar->addAction(QString());
    connect(m_clearTrackAction, &QAction::triggered, this, [this] {
        if (m_localizationSamples.isEmpty()) return;
        if (QMessageBox::question(
                this, tx("清除轨迹", "Clear track"),
                tx("将清除本次完整采样和地图轨迹。尚未导出的数据将无法恢复，是否继续？",
                   "This removes the complete session and map track. Unexported data cannot be recovered. Continue?"))
            == QMessageBox::Yes) {
            clearSamplingData();
        }
    });
    m_exportSamplesAction = m_toolbar->addAction(QString(), this, &MainWindow::exportSamplingCsv);
    m_exportSamplesAction->setEnabled(false);
    m_toolbar->addSeparator();
    m_uploadAction = m_toolbar->addAction(QString(), this, [this] { chooseAndUpload(false); });
    m_uploadSwitchAction = m_toolbar->addAction(QString(), this,
                                                 [this] { chooseAndUpload(true); });
    auto *spacer = new QWidget(m_toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolbar->addWidget(spacer);
    m_languageLabel = new QLabel(m_toolbar);
    m_toolbar->addWidget(m_languageLabel);
    m_languageCombo = new QComboBox(m_toolbar);
    m_languageCombo->addItem(QStringLiteral("中文"), QStringLiteral("zh"));
    m_languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    m_languageCombo->setCurrentIndex(m_english ? 1 : 0);
    m_toolbar->addWidget(m_languageCombo);
    connect(m_languageCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_english = index == 1;
        QSettings().setValue(QStringLiteral("language"), m_english ? QStringLiteral("en")
                                                                  : QStringLiteral("zh"));
        retranslateUi();
    });

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    m_table = new QTableWidget(0, ColumnCount, central);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(Result, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);

    m_batchLabel = new QLabel(central);
    m_log = new QPlainTextEdit(central);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1500);

    m_mapWindow = new QWidget(this, Qt::Window);
    m_mapWindow->setWindowIcon(windowIcon());
    auto *mapLayout = new QVBoxLayout(m_mapWindow);
    mapLayout->setContentsMargins(10, 8, 10, 10);
    auto *mapControls = new QHBoxLayout;
    m_fitMapButton = new QPushButton(m_mapWindow);
    m_closeMapButton = new QPushButton(m_mapWindow);
    mapControls->addWidget(m_fitMapButton);
    mapControls->addStretch();
    mapControls->addWidget(m_closeMapButton);
    connect(m_fitMapButton, &QPushButton::clicked, this, [this] { m_mapView->fitMap(); });
    connect(m_closeMapButton, &QPushButton::clicked, this, [this] {
        m_mapWindow->hide();
        raise();
        activateWindow();
    });
    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), m_mapWindow);
    connect(escapeShortcut, &QShortcut::activated, m_mapWindow, &QWidget::showNormal);
    m_mapInfoLabel = new QLabel(m_mapWindow);
    m_livePositionLabel = new QLabel(m_mapWindow);
    m_heatmapSummaryLabel = new QLabel(m_mapWindow);
    m_heatmapSummaryLabel->setWordWrap(true);
    auto *heatmapControls = new QHBoxLayout;
    m_importHeatmapButton = new QPushButton(m_mapWindow);
    m_toggleHeatmapButton = new QPushButton(m_mapWindow);
    m_toggleHeatmapButton->setCheckable(true);
    m_toggleHeatmapButton->setEnabled(false);
    m_clearHeatmapButton = new QPushButton(m_mapWindow);
    m_clearHeatmapButton->setEnabled(false);
    heatmapControls->addWidget(m_importHeatmapButton);
    heatmapControls->addWidget(m_toggleHeatmapButton);
    heatmapControls->addWidget(m_clearHeatmapButton);
    heatmapControls->addStretch();
    connect(m_importHeatmapButton, &QPushButton::clicked,
            this, &MainWindow::importHeatmapCsv);
    connect(m_toggleHeatmapButton, &QPushButton::toggled,
            this, [this](bool visible) {
        m_mapView->setHeatmapVisible(visible);
        m_mapView->setRobotTrackVisible(!visible);
        m_toggleHeatmapButton->setText(visible
            ? tx("显示轨迹", "Show track")
            : tx("显示热力图", "Show heatmap"));
    });
    connect(m_clearHeatmapButton, &QPushButton::clicked,
            this, &MainWindow::clearHeatmapData);
    m_mapView = new MapView(m_mapWindow);
    mapLayout->addLayout(mapControls);
    mapLayout->addWidget(m_mapInfoLabel);
    mapLayout->addWidget(m_livePositionLabel);
    mapLayout->addLayout(heatmapControls);
    mapLayout->addWidget(m_heatmapSummaryLabel);
    mapLayout->addWidget(m_mapView, 1);

    m_tabs = new QTabWidget(central);
    auto *samplingPage = new QWidget(m_tabs);
    auto *samplingLayout = new QVBoxLayout(samplingPage);
    samplingLayout->setContentsMargins(4, 4, 4, 4);
    m_samplingSummaryLabel = new QLabel(samplingPage);
    m_samplingTable = new QTableWidget(0, 11, samplingPage);
    m_samplingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_samplingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_samplingTable->setAlternatingRowColors(true);
    m_samplingTable->verticalHeader()->setVisible(false);
    m_samplingTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_samplingTable->horizontalHeader()->setSectionResizeMode(10, QHeaderView::Stretch);
    samplingLayout->addWidget(m_samplingSummaryLabel);
    samplingLayout->addWidget(m_samplingTable, 1);
    m_tabs->addTab(samplingPage, QString());
    m_tabs->addTab(m_log, QString());

    layout->addWidget(m_table, 3);
    layout->addWidget(m_batchLabel);
    layout->addWidget(m_tabs, 2);
    setCentralWidget(central);

    m_locationTimer = new QTimer(this);
    m_locationTimer->setInterval(500);
    connect(m_locationTimer, &QTimer::timeout, this, &MainWindow::pollLivePosition);
    retranslateUi();
}

QString MainWindow::tx(const char *chinese, const char *english) const
{
    return QString::fromUtf8(m_english ? english : chinese);
}

QString MainWindow::localizedError(const QString &error) const
{
    if (!m_english) return error;
    if (error == QStringLiteral("协议头不足 16 字节")) return QStringLiteral("Protocol header is shorter than 16 bytes");
    if (error == QStringLiteral("协议魔数不是 5A 01")) return QStringLiteral("Protocol magic is not 5A 01");
    if (error == QStringLiteral("响应序列号不匹配")) return QStringLiteral("Response sequence does not match");
    if (error == QStringLiteral("响应编号不匹配")) return QStringLiteral("Response command does not match");
    if (error == QStringLiteral("连接或响应超时")) return QStringLiteral("Connection or response timed out");
    return error;
}

void MainWindow::retranslateUi()
{
    setWindowTitle(tx("AMR 地图批量管理器 %1", "AMR Map Manager %1")
                       .arg(QCoreApplication::applicationVersion()));
    m_toolbar->setWindowTitle(tx("操作", "Actions"));
    m_addAction->setText(tx("添加机器人", "Add robot"));
    m_removeAction->setText(tx("删除所选", "Remove selected"));
    m_refreshAction->setText(tx("刷新状态", "Refresh status"));
    m_downloadAction->setText(tx("下载当前地图", "Download current map"));
    m_openMapAction->setText(tx("打开地图", "Open map"));
    m_showMapAction->setText(tx("地图窗口", "Map window"));
    m_mapWindow->setWindowTitle(tx("地图与置信度热力图", "Map and confidence heatmap"));
    m_fitMapButton->setText(tx("适应地图", "Fit map"));
    m_closeMapButton->setText(tx("返回主窗口", "Back to main window"));
    m_livePositionAction->setText(m_livePositionAction->isChecked()
        ? tx("停止实时置信度", "Stop live confidence")
        : tx("实时置信度", "Live confidence"));
    m_clearTrackAction->setText(tx("清除轨迹", "Clear track"));
    m_exportSamplesAction->setText(tx("导出采样", "Export samples"));
    m_importHeatmapButton->setText(tx("导入采样 CSV", "Import sample CSV"));
    m_toggleHeatmapButton->setText(m_toggleHeatmapButton->isChecked()
        ? tx("显示轨迹", "Show track")
        : tx("显示热力图", "Show heatmap"));
    m_clearHeatmapButton->setText(tx("清除热力图", "Clear heatmap"));
    updateHeatmapSummary();
    m_uploadAction->setText(tx("仅上传", "Upload only"));
    m_uploadSwitchAction->setText(tx("上传、验证并切换", "Upload, verify and switch"));
    m_languageLabel->setText(tx("语言：", "Language: "));
    m_table->setHorizontalHeaderLabels({tx("选择", "Select"), tx("名称", "Name"),
        QStringLiteral("IP"), tx("连接", "Connection"), tx("任务", "Task"),
        tx("当前地图", "Current map"), QStringLiteral("MD5"), tx("结果", "Result")});
    if (m_activeOperations == 0 && m_pendingRows.isEmpty())
        m_batchLabel->setText(tx("就绪。批量操作默认并发 3 台，繁忙机器人会跳过。",
                                 "Ready. Up to 3 robots run concurrently; busy robots are skipped."));
    m_log->setPlaceholderText(tx("操作日志", "Operation log"));
    m_tabs->setTabText(0, tx("定位采样", "Localization samples"));
    m_tabs->setTabText(1, tx("操作日志", "Operation log"));
    m_samplingTable->setHorizontalHeaderLabels({QStringLiteral("#"),
        tx("时间", "Time"), QStringLiteral("X"), QStringLiteral("Y"),
        tx("朝向", "Heading"), tx("置信度", "Confidence"),
        tx("定位方式", "Method"), tx("位移", "Distance"), tx("速度", "Speed"),
        tx("评分变化", "Score change"), tx("状态", "Status")});
    updateSamplingSummary();
    if (m_livePositionAction->isChecked()) {
        m_livePositionLabel->setText(tx("正在读取实时位置与置信度…",
                                        "Reading live position and confidence..."));
    } else {
        m_livePositionLabel->setText(tx("实时置信度已停止。",
                                        "Live confidence is stopped."));
    }
    updateMapSummary();
    statusBar()->showMessage(tx("真实切图前，请先在单台测试机器人验证端口和协议版本。",
                                "Verify ports and protocol version on one test robot before a real map switch."));
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (auto *online = m_table->item(row, Online); online && online->data(Qt::UserRole).isValid()) {
            switch (online->data(Qt::UserRole).toInt()) {
            case 0: setCell(row, Online, tx("未检查", "Not checked")); break;
            case 1: setCell(row, Online, tx("在线", "Online")); break;
            case 2: setCell(row, Online, tx("离线", "Offline")); break;
            case 3: setCell(row, Online, tx("协议错误", "Protocol error")); break;
            }
        }
        if (auto *task = m_table->item(row, Task); task && task->data(Qt::UserRole).isValid())
            setCell(row, Task, statusName(task->data(Qt::UserRole).toInt(), m_english));
        if (auto *result = m_table->item(row, Result);
            result && result->data(Qt::UserRole).toInt() == 1) {
            setCell(row, Result, tx("等待操作", "Waiting"));
            m_table->item(row, Result)->setData(Qt::UserRole, 1);
        } else if (result) setCell(row, Result, knownResultText(result->text(), m_english));
    }
    for (int row = 0; row < m_samplingTable->rowCount(); ++row) {
        if (auto *method = m_samplingTable->item(row, 6))
            method->setText(localizationMethodName(method->data(Qt::UserRole).toInt(), m_english));
        if (auto *status = m_samplingTable->item(row, 10))
            status->setText(sampleAnomalyText(status->data(Qt::UserRole).toInt()));
    }
}

void MainWindow::loadRobots()
{
    QSettings settings;
    const int count = settings.beginReadArray(QStringLiteral("robots"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *check = new QCheckBox;
        auto *box = new QWidget;
        auto *boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(0, 0, 0, 0);
        boxLayout->setAlignment(Qt::AlignCenter);
        boxLayout->addWidget(check);
        m_table->setCellWidget(row, Selected, box);
        setCell(row, Name, settings.value(QStringLiteral("name")).toString());
        setCell(row, Ip, settings.value(QStringLiteral("host")).toString());
        setCell(row, Online, tx("未检查", "Not checked"));
        m_table->item(row, Online)->setData(Qt::UserRole, 0);
        setCell(row, Task, tx("未知", "Unknown"));
        m_table->item(row, Task)->setData(Qt::UserRole, -1);
        setCell(row, CurrentMap, QStringLiteral("-"));
        setCell(row, Md5, QStringLiteral("-"));
        setCell(row, Result, tx("等待操作", "Waiting"));
        m_table->item(row, Result)->setData(Qt::UserRole, 1);
    }
    settings.endArray();
}

void MainWindow::saveRobots() const
{
    QSettings settings;
    settings.remove(QStringLiteral("robots"));
    settings.beginWriteArray(QStringLiteral("robots"));
    for (int row = 0; row < m_table->rowCount(); ++row) {
        settings.setArrayIndex(row);
        settings.setValue(QStringLiteral("name"), m_table->item(row, Name)->text());
        settings.setValue(QStringLiteral("host"), m_table->item(row, Ip)->text());
    }
    settings.endArray();
}

void MainWindow::addRobot()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tx("手动添加 AMR", "Add AMR manually"));
    QFormLayout form(&dialog);
    QLineEdit name;
    QLineEdit host;
    name.setPlaceholderText(tx("例如 AMR-01", "For example, AMR-01"));
    host.setPlaceholderText(tx("例如 172.17.5.2", "For example, 172.17.5.2"));
    form.addRow(tx("名称", "Name"), &name);
    form.addRow(tx("IPv4 地址", "IPv4 address"), &host);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    QHostAddress address;
    if (name.text().trimmed().isEmpty() || !address.setAddress(host.text().trimmed())
        || address.protocol() != QAbstractSocket::IPv4Protocol) {
        QMessageBox::warning(this, tx("无法添加", "Cannot add robot"),
                             tx("请输入名称和有效的 IPv4 地址。",
                                "Enter a name and a valid IPv4 address."));
        return;
    }
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, Ip)->text() == host.text().trimmed()) {
            QMessageBox::warning(this, tx("重复地址", "Duplicate address"),
                                 tx("该 IP 已经存在。", "This IP address already exists."));
            return;
        }
    }

    const int row = m_table->rowCount();
    m_table->insertRow(row);
    auto *check = new QCheckBox;
    check->setChecked(true);
    auto *box = new QWidget;
    auto *boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    boxLayout->setAlignment(Qt::AlignCenter);
    boxLayout->addWidget(check);
    m_table->setCellWidget(row, Selected, box);
    setCell(row, Name, name.text().trimmed());
    setCell(row, Ip, host.text().trimmed());
    setCell(row, Online, tx("未检查", "Not checked"));
    m_table->item(row, Online)->setData(Qt::UserRole, 0);
    setCell(row, Task, tx("未知", "Unknown"));
    m_table->item(row, Task)->setData(Qt::UserRole, -1);
    setCell(row, CurrentMap, QStringLiteral("-"));
    setCell(row, Md5, QStringLiteral("-"));
    setCell(row, Result, tx("等待操作", "Waiting"));
    m_table->item(row, Result)->setData(Qt::UserRole, 1);
    saveRobots();
}

void MainWindow::removeSelected()
{
    if (m_activeOperations > 0 || !m_pendingRows.isEmpty()) {
        QMessageBox::warning(this, tx("操作进行中", "Operation in progress"),
                             tx("批量操作完成前不能删除机器人。",
                                "Robots cannot be removed until the batch operation finishes."));
        return;
    }
    const auto rows = selectedRows();
    if (m_livePositionRow >= 0 && rows.contains(m_livePositionRow)) stopLivePosition();
    for (auto it = rows.crbegin(); it != rows.crend(); ++it) m_table->removeRow(*it);
    saveRobots();
}

void MainWindow::startLivePosition()
{
    if (!m_mapView->hasMap()) {
        QMessageBox::information(this, tx("请先打开地图", "Open a map first"),
                                 tx("请先打开或下载机器人当前地图。",
                                    "Open or download the robot's current map first."));
        stopLivePosition();
        return;
    }
    const QList<int> rows = selectedRows();
    if (rows.size() != 1) {
        QMessageBox::information(this, tx("选择机器人", "Select a robot"),
                                 tx("实时置信度只能显示一台机器人，请只勾选一台。",
                                    "Live confidence can display one robot. Select exactly one robot."));
        stopLivePosition();
        return;
    }

    showMapWindow();
    m_livePositionRow = rows.first();
    m_livePositionFailures = 0;
    m_locationRequestPending = true;
    const int session = ++m_livePositionSession;
    m_livePositionAction->setText(tx("停止实时置信度", "Stop live confidence"));
    m_livePositionLabel->setText(tx("正在确认机器人当前地图…",
                                    "Checking the robot's current map..."));

    const Robot robot = robotAt(m_livePositionRow);
    query(m_livePositionRow, robot.statusPort, RbkProtocol::QueryMap, {},
          [this, session](RequestResult result) {
        if (session != m_livePositionSession) return;
        m_locationRequestPending = false;
        QJsonObject json;
        QString error;
        if (!result.ok || !parseJson(result.payload, &json, &error)
            || !responseSucceeded(json, &error)) {
            const QString message = result.ok ? error : localizedError(result.error);
            QMessageBox::warning(mapDialogParent(), tx("无法启动实时置信度", "Cannot start live confidence"),
                                 message);
            stopLivePosition();
            return;
        }

        const QString currentMap = json.value(QStringLiteral("current_map")).toString();
        const QString currentMd5 = json.value(QStringLiteral("current_map_md5")).toString();
        setCell(m_livePositionRow, CurrentMap, currentMap.isEmpty() ? QStringLiteral("-") : currentMap);
        setCell(m_livePositionRow, Md5, currentMd5.isEmpty() ? QStringLiteral("-") : currentMd5);
        const QString previewMap = normalizedMapName(m_mapSummary.name);
        if (!previewMap.isEmpty() && normalizedMapName(currentMap) != previewMap) {
            QMessageBox::warning(mapDialogParent(), tx("地图不一致", "Map mismatch"),
                tx("机器人当前地图是 %1，预览地图是 %2。\n"
                   "请打开该机器人的当前地图后再启动。",
                   "The robot is using map %1, while the preview shows %2.\n"
                   "Open the robot's current map before starting live confidence.")
                    .arg(currentMap, m_mapSummary.name));
            stopLivePosition();
            return;
        }

        const Robot samplingRobot = robotAt(m_livePositionRow);
        const QString sessionMap = normalizedMapName(currentMap);
        const bool differentSession =
            (!m_samplingRobotHost.isEmpty() && m_samplingRobotHost != samplingRobot.host)
            || (!m_samplingMapName.isEmpty()
                && normalizedMapName(m_samplingMapName) != sessionMap);
        if (differentSession && !m_localizationSamples.isEmpty()
            && QMessageBox::question(
                   mapDialogParent(), tx("开始新的采样", "Start a new sampling session"),
                   tx("现有完整采样属于另一台机器人或地图。开始新采样会清除现有数据，"
                      "请先导出需要保留的数据。是否继续？",
                      "The complete samples belong to another robot or map. Starting a new session "
                      "will clear them. Export any data you need first. Continue?"))
                != QMessageBox::Yes) {
            stopLivePosition();
            return;
        }
        if (differentSession) {
            clearSamplingData();
        }
        m_samplingRobotName = samplingRobot.name;
        m_samplingRobotHost = samplingRobot.host;
        m_samplingMapName = currentMap.isEmpty() ? m_mapSummary.name : currentMap;
        updateSamplingSummary();

        m_locationTimer->start();
        pollLivePosition();
    }, 5000);
}

void MainWindow::stopLivePosition(bool clearMarker)
{
    ++m_livePositionSession;
    if (m_locationTimer) m_locationTimer->stop();
    m_locationRequestPending = false;
    m_livePositionRow = -1;
    m_livePositionFailures = 0;
    if (clearMarker && m_mapView) m_mapView->clearRobotPose();
    if (m_livePositionAction) {
        const QSignalBlocker blocker(m_livePositionAction);
        m_livePositionAction->setChecked(false);
        m_livePositionAction->setText(tx("实时置信度", "Live confidence"));
    }
    if (m_livePositionLabel) {
        m_livePositionLabel->setText(tx("实时置信度已停止。",
                                        "Live confidence is stopped."));
    }
}

void MainWindow::pollLivePosition()
{
    if (m_livePositionRow < 0 || m_locationRequestPending
        || !m_livePositionAction->isChecked()) return;

    const int row = m_livePositionRow;
    const int session = m_livePositionSession;
    const Robot robot = robotAt(row);
    m_locationRequestPending = true;
    query(row, robot.statusPort, RbkProtocol::QueryLocation, {},
          [this, row, session, robot](RequestResult result) {
        if (session != m_livePositionSession) return;
        m_locationRequestPending = false;

        QJsonObject json;
        QString error;
        if (!result.ok || !parseJson(result.payload, &json, &error)
            || !responseSucceeded(json, &error)
            || !json.value(QStringLiteral("x")).isDouble()
            || !json.value(QStringLiteral("y")).isDouble()
            || !json.value(QStringLiteral("angle")).isDouble()) {
            ++m_livePositionFailures;
            ++m_failedLocationReads;
            const QString message = !result.ok ? localizedError(result.error)
                : (!error.isEmpty() ? error : tx("位置响应缺少 x、y 或 angle。",
                                                "The location response is missing x, y, or angle."));
            m_livePositionLabel->setText(tx("实时位置读取失败（%1）：%2",
                                            "Live position read failed (%1): %2")
                                             .arg(m_livePositionFailures).arg(message));
            if (m_livePositionFailures >= 3) m_mapView->clearRobotPose();
            updateSamplingSummary();
            return;
        }

        m_livePositionFailures = 0;
        const double x = json.value(QStringLiteral("x")).toDouble();
        const double y = json.value(QStringLiteral("y")).toDouble();
        const double angle = json.value(QStringLiteral("angle")).toDouble();
        double confidence = std::numeric_limits<double>::quiet_NaN();
        if (json.value(QStringLiteral("confidence")).isDouble()) {
            const double value = json.value(QStringLiteral("confidence")).toDouble();
            if (value >= 0.0 && value <= 1.0) confidence = value;
        }
        const int method = json.value(QStringLiteral("loc_method")).toInt(-1);
        recordLocalizationSample(robot, x, y, angle, confidence, method);
        const QString confidenceText = std::isfinite(confidence)
            ? QStringLiteral("%1%").arg(confidence * 100.0, 0, 'f', 1)
            : tx("无数据", "N/A");
        const QString methodText = localizationMethodName(method, m_english);
        m_mapView->setRobotPose(x, y, angle, confidence,
                                QStringLiteral("%1  %2").arg(robot.name, confidenceText));
        m_livePositionLabel->setText(
            tx("机器人：%1    X：%2 m    Y：%3 m    朝向：%4 rad    "
               "置信度：%5    定位方式：%6    更新：%7",
               "Robot: %1    X: %2 m    Y: %3 m    Heading: %4 rad    "
               "Confidence: %5    Method: %6    Updated: %7")
                .arg(robot.name)
                .arg(x, 0, 'f', 3)
                .arg(y, 0, 'f', 3)
                .arg(angle, 0, 'f', 3)
                .arg(confidenceText)
                .arg(methodText)
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    }, 2000);
}

void MainWindow::recordLocalizationSample(const Robot &robot, double x, double y,
                                          double angle, double confidence, int method)
{
    LocalizationSample sample;
    sample.timestampMs = QDateTime::currentMSecsSinceEpoch();
    sample.x = x;
    sample.y = y;
    sample.angle = angle;
    sample.confidence = std::isfinite(confidence) ? confidence : -1.0;
    sample.method = method;

    if (m_samplingRobotHost.isEmpty()) {
        m_samplingRobotName = robot.name;
        m_samplingRobotHost = robot.host;
        m_samplingMapName = m_mapSummary.name;
    }

    if (sample.confidence < 0.0) sample.anomalies |= MissingConfidence;
    else if (sample.confidence < LowConfidenceThreshold) sample.anomalies |= LowConfidence;
    if (!m_mapView->containsMapPosition(x, y)) sample.anomalies |= OutsideMap;

    if (!m_localizationSamples.isEmpty()) {
        const LocalizationSample &previous = m_localizationSamples.constLast();
        sample.hasPrevious = true;
        const double elapsed = (sample.timestampMs - previous.timestampMs) / 1000.0;
        sample.distance = std::hypot(sample.x - previous.x, sample.y - previous.y);
        sample.speed = elapsed > 0.0 ? sample.distance / elapsed : 0.0;
        if (sample.distance >= PositionJumpDistance && sample.speed > PositionJumpSpeed)
            sample.anomalies |= PositionJump;
        if (sample.confidence >= 0.0 && previous.confidence >= 0.0) {
            sample.hasConfidenceDelta = true;
            sample.confidenceDelta = sample.confidence - previous.confidence;
            if (sample.confidenceDelta <= -ConfidenceDropThreshold)
                sample.anomalies |= ConfidenceDrop;
        }
    }

    m_localizationSamples.append(sample);
    if (sample.confidence >= 0.0) {
        m_confidenceSum += sample.confidence;
        m_minConfidence = std::min(m_minConfidence, sample.confidence);
        ++m_validConfidenceSamples;
    }
    if (sample.anomalies != 0) ++m_anomalySamples;

    m_mapView->appendRobotTrackSample(sample.x, sample.y, sample.confidence,
                                      sample.anomalies != 0,
                                      (sample.anomalies & PositionJump) != 0);

    if (m_samplingTable->rowCount() >= MaxVisibleSamples) {
        for (int i = 0; i < 200; ++i) m_samplingTable->removeRow(0);
    }
    const int row = m_samplingTable->rowCount();
    m_samplingTable->insertRow(row);
    const auto put = [this, row](int column, const QString &text) {
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(column >= 2 && column <= 9
                                   ? Qt::AlignRight | Qt::AlignVCenter
                                   : Qt::AlignLeft | Qt::AlignVCenter);
        m_samplingTable->setItem(row, column, item);
        return item;
    };
    put(0, QString::number(m_localizationSamples.size()));
    put(1, QDateTime::fromMSecsSinceEpoch(sample.timestampMs)
               .toString(QStringLiteral("HH:mm:ss.zzz")));
    put(2, QString::number(sample.x, 'f', 3));
    put(3, QString::number(sample.y, 'f', 3));
    put(4, QString::number(sample.angle, 'f', 3));
    put(5, sample.confidence >= 0.0
               ? QStringLiteral("%1%").arg(sample.confidence * 100.0, 0, 'f', 1)
               : QStringLiteral("-"));
    auto *methodItem = put(6, localizationMethodName(sample.method, m_english));
    methodItem->setData(Qt::UserRole, sample.method);
    put(7, sample.hasPrevious ? QString::number(sample.distance, 'f', 3)
                              : QStringLiteral("-"));
    put(8, sample.hasPrevious ? QString::number(sample.speed, 'f', 3)
                              : QStringLiteral("-"));
    put(9, sample.hasConfidenceDelta
               ? QStringLiteral("%1%").arg(sample.confidenceDelta * 100.0, 0, 'f', 1)
               : QStringLiteral("-"));
    auto *statusItem = put(10, sampleAnomalyText(sample.anomalies));
    statusItem->setData(Qt::UserRole, sample.anomalies);
    if (sample.anomalies != 0) {
        const QBrush background(QColor(255, 225, 225));
        for (int column = 0; column < m_samplingTable->columnCount(); ++column)
            m_samplingTable->item(row, column)->setBackground(background);
    }
    if (m_tabs->currentIndex() == 0) m_samplingTable->scrollToBottom();
    m_exportSamplesAction->setEnabled(true);
    updateSamplingSummary();
}

QString MainWindow::sampleAnomalyText(int anomalies) const
{
    if (anomalies == 0) return tx("正常", "Normal");
    QStringList labels;
    if (anomalies & LowConfidence) labels << tx("低置信度", "Low confidence");
    if (anomalies & ConfidenceDrop) labels << tx("评分突降", "Score drop");
    if (anomalies & PositionJump) labels << tx("位置跳变", "Position jump");
    if (anomalies & OutsideMap) labels << tx("地图范围外", "Outside map");
    if (anomalies & MissingConfidence) labels << tx("无置信度", "No confidence");
    return labels.join(QStringLiteral("; "));
}

void MainWindow::updateSamplingSummary()
{
    if (!m_samplingSummaryLabel) return;
    if (m_localizationSamples.isEmpty()) {
        if (m_failedLocationReads > 0) {
            m_samplingSummaryLabel->setText(
                tx("尚无成功采样，位置读取失败：%1。",
                   "No successful samples. Location read failures: %1.")
                    .arg(m_failedLocationReads));
        } else {
            m_samplingSummaryLabel->setText(tx(
                "尚无采样。启动实时置信度后，将完整记录本次运行的位置和评分。",
                "No samples yet. Start live confidence to record the complete position and score stream."));
        }
        return;
    }
    const QString average = m_validConfidenceSamples > 0
        ? QStringLiteral("%1%").arg(m_confidenceSum * 100.0 / m_validConfidenceSamples, 0, 'f', 1)
        : QStringLiteral("-");
    const QString minimum = m_validConfidenceSamples > 0
        ? QStringLiteral("%1%").arg(m_minConfidence * 100.0, 0, 'f', 1)
        : QStringLiteral("-");
    m_samplingSummaryLabel->setText(
        tx("机器人：%1 (%2)    地图：%3    完整采样：%4    平均置信度：%5    "
           "最低置信度：%6    异常采样：%7    读取失败：%8",
           "Robot: %1 (%2)    Map: %3    Complete samples: %4    Average confidence: %5    "
           "Minimum confidence: %6    Anomalous samples: %7    Read failures: %8")
            .arg(m_samplingRobotName)
            .arg(m_samplingRobotHost)
            .arg(m_samplingMapName.isEmpty() ? m_mapSummary.name : m_samplingMapName)
            .arg(m_localizationSamples.size())
            .arg(average)
            .arg(minimum)
            .arg(m_anomalySamples)
            .arg(m_failedLocationReads));
}

void MainWindow::clearSamplingData()
{
    m_localizationSamples.clear();
    m_samplingRobotName.clear();
    m_samplingRobotHost.clear();
    m_samplingMapName.clear();
    m_confidenceSum = 0.0;
    m_minConfidence = 1.0;
    m_validConfidenceSamples = 0;
    m_anomalySamples = 0;
    m_failedLocationReads = 0;
    if (m_samplingTable) m_samplingTable->setRowCount(0);
    if (m_mapView) m_mapView->clearRobotTrack();
    if (m_exportSamplesAction) m_exportSamplesAction->setEnabled(false);
    updateSamplingSummary();
}

void MainWindow::exportSamplingCsv()
{
    if (m_localizationSamples.isEmpty()) {
        QMessageBox::information(this, tx("没有采样", "No samples"),
                                 tx("当前没有可以导出的定位采样。",
                                    "There are no localization samples to export."));
        return;
    }
    QString directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (directory.isEmpty()) directory = QDir::homePath();
    QString baseName = m_samplingRobotName;
    baseName.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")), QStringLiteral("_"));
    const QString suggested = QDir(directory).filePath(
        QStringLiteral("localization_%1_%2.csv")
            .arg(baseName, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString fileName = QFileDialog::getSaveFileName(
        this, tx("导出定位采样", "Export localization samples"), suggested,
        tx("CSV 文件 (*.csv)", "CSV files (*.csv)"));
    if (fileName.isEmpty()) return;

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tx("导出失败", "Export failed"), file.errorString());
        return;
    }
    file.write("\xEF\xBB\xBF", 3);
    QTextStream stream(&file);
    const auto csv = [](QString value) {
        value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"") + value + QStringLiteral("\"");
    };
    stream << "index,timestamp,robot,ip,map,x,y,angle,confidence,loc_method,"
              "distance_m,speed_m_s,confidence_delta,anomaly\n";
    for (qsizetype i = 0; i < m_localizationSamples.size(); ++i) {
        const LocalizationSample &sample = m_localizationSamples.at(i);
        stream << (i + 1) << ','
               << csv(QDateTime::fromMSecsSinceEpoch(sample.timestampMs).toString(Qt::ISODateWithMs)) << ','
               << csv(m_samplingRobotName) << ',' << csv(m_samplingRobotHost) << ','
               << csv(m_samplingMapName) << ','
               << QString::number(sample.x, 'f', 6) << ','
               << QString::number(sample.y, 'f', 6) << ','
               << QString::number(sample.angle, 'f', 6) << ',';
        if (sample.confidence >= 0.0) stream << QString::number(sample.confidence, 'f', 6);
        stream << ',' << sample.method << ',';
        if (sample.hasPrevious) {
            stream << QString::number(sample.distance, 'f', 6) << ','
                   << QString::number(sample.speed, 'f', 6);
        } else stream << ',';
        stream << ',';
        if (sample.hasConfidenceDelta)
            stream << QString::number(sample.confidenceDelta, 'f', 6);
        stream << ',' << csv(sampleAnomalyText(sample.anomalies)) << '\n';
    }
    file.close();
    log(tx("定位采样已导出：%1", "Localization samples exported: %1").arg(fileName));
    QMessageBox::information(this, tx("导出完成", "Export complete"), fileName);
}

void MainWindow::importHeatmapCsv()
{
    if (!m_mapView->hasMap() || m_mapSummary.name.trimmed().isEmpty()) {
        QMessageBox::information(mapDialogParent(), tx("请先打开地图", "Open a map first"),
            tx("请先打开对应的机器人地图，再导入定位采样。",
               "Open the matching robot map before importing localization samples."));
        return;
    }
    const QStringList files = QFileDialog::getOpenFileNames(
        mapDialogParent(), tx("导入定位采样", "Import localization samples"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tx("采样 CSV (*.csv)", "Sample CSV files (*.csv)"));
    if (files.isEmpty()) return;

    const int session = ++m_heatmapImportSession;
    const QString mapName = m_mapSummary.name;
    m_importHeatmapButton->setEnabled(false);
    m_heatmapSummaryLabel->setText(tx("正在读取并汇总采样…",
                                      "Reading and aggregating samples..."));
    auto *watcher = new QFutureWatcher<HeatmapImportResult>(this);
    connect(watcher, &QFutureWatcher<HeatmapImportResult>::finished,
            this, [this, watcher, session] {
        const HeatmapImportResult result = watcher->result();
        watcher->deleteLater();
        if (session != m_heatmapImportSession) return;
        m_importHeatmapButton->setEnabled(true);
        if (!result.error.isEmpty()) {
            updateHeatmapSummary();
            QMessageBox::warning(mapDialogParent(), tx("导入失败", "Import failed"), result.error);
            return;
        }
        if (result.cells.isEmpty()) {
            updateHeatmapSummary();
            QMessageBox::information(mapDialogParent(), tx("没有移动样本", "No moving samples"),
                tx("CSV 中没有符合条件的移动采样，无法生成热力图。",
                   "The CSV contains no qualifying moving samples for a heatmap."));
            return;
        }

        m_heatmapCells = result.cells;
        m_heatmapRowCount = result.rows;
        m_heatmapMovingSamples = result.movingSamples;
        m_heatmapRejectedSamples = result.rejectedSamples;
        m_heatmapFiles = result.files;
        m_heatmapRobotName = result.robotName;
        const int renderedCells = m_mapView->setHeatmapCells(m_heatmapCells);
        if (renderedCells == 0) {
            clearHeatmapData();
            QMessageBox::warning(mapDialogParent(), tx("无法显示热力图", "Cannot display heatmap"),
                tx("采样坐标没有落在当前地图的可通行区域，请核对地图版本和坐标。",
                   "The sample coordinates do not fall in free space on this map. "
                   "Check the map version and coordinates."));
            return;
        }
        m_toggleHeatmapButton->setEnabled(true);
        m_toggleHeatmapButton->setChecked(true);
        m_mapView->setHeatmapVisible(true);
        m_mapView->setRobotTrackVisible(false);
        m_clearHeatmapButton->setEnabled(true);
        updateHeatmapSummary();
    });
    watcher->setFuture(QtConcurrent::run([files, mapName] {
        LocalizationHeatmapBuilder builder;
        HeatmapImportResult result;
        for (const QString &file : files) {
            if (!builder.addCsvFile(file, mapName, &result.error)) return result;
        }
        result.cells = builder.finish();
        result.robotName = builder.robotName();
        result.rows = builder.rowCount();
        result.movingSamples = builder.movingSamples();
        result.rejectedSamples = builder.rejectedSamples();
        result.files = builder.fileCount();
        return result;
    }));
}

void MainWindow::clearHeatmapData()
{
    ++m_heatmapImportSession;
    m_heatmapCells.clear();
    m_heatmapRowCount = 0;
    m_heatmapMovingSamples = 0;
    m_heatmapRejectedSamples = 0;
    m_heatmapFiles = 0;
    m_heatmapRobotName.clear();
    if (m_mapView) m_mapView->clearHeatmap();
    if (m_mapView) m_mapView->setRobotTrackVisible(true);
    if (m_toggleHeatmapButton) {
        const QSignalBlocker blocker(m_toggleHeatmapButton);
        m_toggleHeatmapButton->setChecked(false);
        m_toggleHeatmapButton->setEnabled(false);
        m_toggleHeatmapButton->setText(tx("显示热力图", "Show heatmap"));
    }
    if (m_clearHeatmapButton) m_clearHeatmapButton->setEnabled(false);
    if (m_importHeatmapButton) m_importHeatmapButton->setEnabled(true);
    updateHeatmapSummary();
}

void MainWindow::updateHeatmapSummary()
{
    if (!m_heatmapSummaryLabel) return;
    if (m_heatmapCells.isEmpty()) {
        m_heatmapSummaryLabel->setText(tx(
            "导入同一机器人、同一地图的采样 CSV 后显示热力图。淡色表示只经过一次；空白表示未采样。地图名称相同也请核对版本。",
            "Import sample CSV files from the same robot and map. Pale areas have one visit; blank areas have no samples. Verify the map revision even if its name matches."));
        return;
    }
    int confirmed = 0;
    int repeatedLow = 0;
    for (const ConfidenceHeatmapCell &cell : m_heatmapCells) {
        if (cell.visits < 2) continue;
        ++confirmed;
        if (cell.lowVisitRatio >= 0.6) ++repeatedLow;
    }
    m_heatmapSummaryLabel->setText(
        tx("%1：%2 个 CSV，%3 条原始记录，%4 条移动采样；0.5 m 网格 %5 个，"
           "重复经过 %6 个，重复低分 %7 个，剔除 %8 条。红<60%，黄60–80%，绿≥80%；淡色=一次经过。",
           "%1: %2 CSV files, %3 raw rows, %4 moving samples; %5 cells at 0.5 m, "
           "%6 revisited, %7 repeatedly low, %8 rejected. Red <60%, amber 60–80%, "
           "green ≥80%; pale = one visit.")
            .arg(m_heatmapRobotName)
            .arg(m_heatmapFiles)
            .arg(m_heatmapRowCount)
            .arg(m_heatmapMovingSamples)
            .arg(m_heatmapCells.size())
            .arg(confirmed)
            .arg(repeatedLow)
            .arg(m_heatmapRejectedSamples));
}

void MainWindow::refreshSelected()
{
    auto rows = selectedRows();
    if (rows.isEmpty()) {
        for (int row = 0; row < m_table->rowCount(); ++row) rows.append(row);
    }
    for (const int row : rows) {
        setCell(row, Result, tx("查询地图状态…", "Querying map status..."));
        const Robot robot = robotAt(row);
        query(row, robot.statusPort, RbkProtocol::QueryMap, {}, [this, row](RequestResult result) {
            if (!result.ok) {
                setCell(row, Online, tx("离线", "Offline"));
                m_table->item(row, Online)->setData(Qt::UserRole, 2);
                setCell(row, Result, localizedError(result.error));
                return;
            }
            QJsonObject json;
            QString error;
            if (!parseJson(result.payload, &json, &error) || !responseSucceeded(json, &error)) {
                setCell(row, Online, tx("协议错误", "Protocol error"));
                m_table->item(row, Online)->setData(Qt::UserRole, 3);
                setCell(row, Result, error);
                return;
            }
            setCell(row, Online, tx("在线", "Online"));
            m_table->item(row, Online)->setData(Qt::UserRole, 1);
            setCell(row, CurrentMap, json.value(QStringLiteral("current_map")).toString(QStringLiteral("-")));
            setCell(row, Md5, json.value(QStringLiteral("current_map_md5")).toString(QStringLiteral("-")));
            setCell(row, Result, tx("状态已刷新", "Status refreshed"));
        });
    }
}

void MainWindow::downloadMap()
{
    const auto rows = selectedRows();
    if (rows.size() != 1) {
        QMessageBox::information(this, tx("选择机器人", "Select a robot"),
                                 tx("下载地图时请只选择一台机器人。",
                                    "Select exactly one robot to download its current map."));
        return;
    }
    if (m_livePositionAction->isChecked()) stopLivePosition();
    const int row = rows.first();
    resolveDownloadMap(row);
}

void MainWindow::resolveDownloadMap(int row)
{
    setCell(row, Result, tx("查找当前地图的实际文件…",
                            "Resolving the current map's stored file..."));
    const Robot robot = robotAt(row);
    query(row, robot.statusPort, RbkProtocol::QueryMap, {},
          [this, row](RequestResult result) {
        if (!result.ok) {
            setCell(row, Result, tx("地图列表查询失败：%1", "Map list query failed: %1")
                                     .arg(localizedError(result.error)));
            return;
        }

        QJsonObject json;
        QString error;
        if (!parseJson(result.payload, &json, &error) || !responseSucceeded(json, &error)) {
            setCell(row, Result, tx("地图列表查询失败：%1", "Map list query failed: %1")
                                     .arg(error));
            return;
        }

        const QString currentMap = json.value(QStringLiteral("current_map")).toString().trimmed();
        const QString currentMd5 = json.value(QStringLiteral("current_map_md5")).toString().trimmed();
        setCell(row, CurrentMap, currentMap.isEmpty() ? QStringLiteral("-") : currentMap);
        setCell(row, Md5, currentMd5.isEmpty() ? QStringLiteral("-") : currentMd5);
        if (currentMd5.isEmpty()) {
            setCell(row, Result, tx("机器人未返回当前地图 MD5。",
                                    "The robot did not return the current map MD5."));
            return;
        }

        QStringList storedFiles;
        const QJsonArray fileInfo = json.value(QStringLiteral("map_files_info")).toArray();
        for (const QJsonValue &value : fileInfo) {
            const QString name = value.toObject().value(QStringLiteral("name")).toString().trimmed();
            if (!name.isEmpty()) storedFiles.append(name);
        }
        if (storedFiles.isEmpty()) {
            const QJsonArray maps = json.value(QStringLiteral("maps")).toArray();
            for (const QJsonValue &value : maps) {
                QString name = value.toString().trimmed();
                if (name.isEmpty()) continue;
                if (!name.endsWith(QStringLiteral(".smap"), Qt::CaseInsensitive)) {
                    name += QStringLiteral(".smap");
                }
                storedFiles.append(name);
            }
        }
        storedFiles.removeDuplicates();
        if (storedFiles.isEmpty()) {
            setCell(row, Result, tx("机器人地图列表为空。", "The robot map list is empty."));
            return;
        }

        findDownloadMapByMd5(row, storedFiles, currentMd5);
    });
}

void MainWindow::findDownloadMapByMd5(int row, const QStringList &storedFiles,
                                      const QString &expectedMd5, int fallbackIndex)
{
    if (fallbackIndex >= storedFiles.size()) {
        setCell(row, Result,
                tx("找不到与当前地图 MD5 匹配的存储文件。",
                   "No stored file matches the current map MD5."));
        return;
    }

    QJsonArray requestedNames;
    if (fallbackIndex < 0) {
        for (const QString &name : storedFiles) requestedNames.append(name);
        setCell(row, Result, tx("按 MD5 定位当前地图…",
                                "Locating the current map by MD5..."));
    } else {
        requestedNames.append(storedFiles.at(fallbackIndex));
        setCell(row, Result,
                tx("逐个核对地图文件（%1/%2）…",
                   "Checking map files individually (%1/%2)...")
                    .arg(fallbackIndex + 1)
                    .arg(storedFiles.size()));
    }

    const Robot robot = robotAt(row);
    query(row, robot.statusPort, RbkProtocol::QueryMapMd5,
          compactJson({{QStringLiteral("map_names"), requestedNames}}),
          [this, row, storedFiles, expectedMd5, fallbackIndex](RequestResult result) {
        if (!result.ok) {
            setCell(row, Result, tx("MD5 查询失败：%1", "MD5 query failed: %1")
                                     .arg(localizedError(result.error)));
            return;
        }

        QJsonObject json;
        QString error;
        if (!parseJson(result.payload, &json, &error) || !responseSucceeded(json, &error)) {
            if (fallbackIndex < 0) {
                log(tx("批量 MD5 查询失败，改为逐个查询：%1",
                       "Batch MD5 query failed; retrying files individually: %1").arg(error));
                findDownloadMapByMd5(row, storedFiles, expectedMd5, 0);
            } else {
                log(tx("跳过无法查询的地图文件 %1：%2",
                       "Skipping map file %1 because it cannot be queried: %2")
                        .arg(storedFiles.at(fallbackIndex), error));
                findDownloadMapByMd5(row, storedFiles, expectedMd5, fallbackIndex + 1);
            }
            return;
        }

        const QJsonArray info = json.value(QStringLiteral("map_info")).toArray();
        for (const QJsonValue &value : info) {
            const QJsonObject item = value.toObject();
            if (item.value(QStringLiteral("md5")).toString().compare(
                    expectedMd5, Qt::CaseInsensitive) != 0) {
                continue;
            }
            QString storedFileName = item.value(QStringLiteral("name")).toString().trimmed();
            if (storedFileName.isEmpty() && fallbackIndex >= 0) {
                storedFileName = storedFiles.at(fallbackIndex);
            }
            if (!storedFileName.isEmpty()) {
                log(tx("当前地图实际存储文件：%1",
                       "Current map's stored file: %1").arg(storedFileName));
                downloadStoredMap(row, storedFileName);
                return;
            }
        }

        if (fallbackIndex < 0) {
            setCell(row, Result,
                    tx("找不到与当前地图 MD5 匹配的存储文件。",
                       "No stored file matches the current map MD5."));
        } else {
            findDownloadMapByMd5(row, storedFiles, expectedMd5, fallbackIndex + 1);
        }
    });
}

void MainWindow::downloadStoredMap(int row, const QString &storedFileName)
{
    QString requestName = storedFileName.trimmed();
    if (requestName.endsWith(QStringLiteral(".smap"), Qt::CaseInsensitive)) requestName.chop(5);
    if (requestName.isEmpty()) {
        setCell(row, Result, tx("机器人返回的地图文件名为空。",
                                "The robot returned an empty map filename."));
        return;
    }

    QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (directory.isEmpty()) directory = QDir::homePath();
    QString localBaseName = QFileInfo(storedFileName).fileName();
    if (!localBaseName.endsWith(QStringLiteral(".smap"), Qt::CaseInsensitive)) {
        localBaseName += QStringLiteral(".smap");
    }
    localBaseName.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")),
                          QStringLiteral("_"));
    QString fileName = QDir(directory).filePath(localBaseName);
    if (QFileInfo::exists(fileName)) {
        const QFileInfo info(localBaseName);
        fileName = QDir(directory).filePath(QStringLiteral("%1_%2.%3").arg(
            info.completeBaseName(),
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")),
            info.suffix().isEmpty() ? QStringLiteral("smap") : info.suffix()));
    }
    setCell(row, Result, tx("下载当前地图…", "Downloading current map..."));
    log(tx("下载机器人存储地图 %1（API map_name: %2）",
           "Downloading robot map %1 (API map_name: %2)")
            .arg(storedFileName, requestName));
    const Robot robot = robotAt(row);
    query(row, robot.configPort, RbkProtocol::DownloadMap,
          compactJson({{QStringLiteral("map_name"), requestName}}),
          [this, row, fileName, requestName](RequestResult result) {
        if (!result.ok) { setCell(row, Result, localizedError(result.error)); return; }
        QJsonObject possibleError;
        QString ignored;
        if (parseJson(result.payload, &possibleError, &ignored)
            && possibleError.contains(QStringLiteral("ret_code"))
            && possibleError.value(QStringLiteral("ret_code")).toInt() != 0) {
            const QString robotError = possibleError.value(QStringLiteral("err_msg")).toString(
                tx("机器人拒绝下载", "The robot rejected the download"));
            setCell(row, Result,
                    tx("下载地图 %1 失败：%2", "Download of map %1 failed: %2")
                        .arg(requestName, robotError));
            return;
        }
        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly) || file.write(result.payload) != result.payload.size()) {
            setCell(row, Result, tx("无法保存文件：%1", "Cannot save file: %1")
                                     .arg(file.errorString()));
            return;
        }
        file.close();
        const QString md5 = QString::fromLatin1(QCryptographicHash::hash(
            result.payload, QCryptographicHash::Md5).toHex());
        setCell(row, Result, tx("下载完成，本地 MD5 %1", "Download complete. Local MD5: %1").arg(md5));
        log(tx("地图已保存：%1", "Map saved to: %1").arg(fileName));
        MapSummary summary;
        QString previewError;
        bool previewReady = false;
        if (m_mapView->loadBytes(result.payload, &summary, &previewError)) {
            m_mapSummary = summary;
            m_hasMapSummary = true;
            clearHeatmapData();
            updateMapSummary();
            previewReady = true;
        } else {
            log(tx("地图已下载，但预览解析失败：%1",
                   "The map was downloaded, but preview parsing failed: %1").arg(previewError));
        }
        QMessageBox::information(this, tx("下载完成", "Download complete"),
            tx("当前地图已保存到：\n%1\n\n如需改名，请在文件管理器中操作。",
               "The current map was saved to:\n%1\n\nRename it later in your file manager if needed.")
                .arg(fileName));
        if (previewReady) showMapWindow(true);
    }, 60000);
}

void MainWindow::openMapFile()
{
    const QString fileName = QFileDialog::getOpenFileName(this,
        tx("打开地图", "Open map"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        tx("SEER 地图 (*.smap);;JSON 文件 (*.json);;所有文件 (*)",
           "SEER maps (*.smap);;JSON files (*.json);;All files (*)"));
    if (fileName.isEmpty()) return;

    if (m_livePositionAction->isChecked()) stopLivePosition();

    MapSummary summary;
    QString error;
    if (!m_mapView->loadFile(fileName, &summary, &error)) {
        QMessageBox::critical(this, tx("无法打开地图", "Cannot open map"),
                              tx("地图解析失败：%1", "Map parsing failed: %1").arg(error));
        return;
    }
    m_mapSummary = summary;
    m_hasMapSummary = true;
    clearHeatmapData();
    updateMapSummary();
    showMapWindow(true);
    log(tx("已打开地图：%1", "Opened map: %1").arg(fileName));
}

void MainWindow::showMapWindow(bool fit)
{
    if (!m_mapWindow) return;
    m_mapWindow->showFullScreen();
    m_mapWindow->raise();
    m_mapWindow->activateWindow();
    if (fit && m_mapView->hasMap()) {
        QTimer::singleShot(0, m_mapView, [this] { m_mapView->fitMap(); });
    }
}

QWidget *MainWindow::mapDialogParent()
{
    return m_mapWindow && m_mapWindow->isVisible() ? m_mapWindow : this;
}

void MainWindow::updateMapSummary()
{
    if (!m_mapInfoLabel) return;
    if (!m_hasMapSummary) {
        m_mapInfoLabel->setText(tx("尚未打开地图。可点击“打开地图”，或下载机器人当前地图后自动预览。",
                                   "No map is open. Click Open map, or download a robot's current map for automatic preview."));
        return;
    }
    m_mapInfoLabel->setText(
        tx("地图：%1    类型：%2    版本：%3    分辨率：%4 m    扫描点：%5    站点：%6    路径：%7    区域：%8",
           "Map: %1    Type: %2    Version: %3    Resolution: %4 m    Scan points: %5    Stations: %6    Paths: %7    Areas: %8")
            .arg(m_mapSummary.name.isEmpty() ? QStringLiteral("-") : m_mapSummary.name,
                 m_mapSummary.type.isEmpty() ? QStringLiteral("-") : m_mapSummary.type,
                 m_mapSummary.version.isEmpty() ? QStringLiteral("-") : m_mapSummary.version)
            .arg(m_mapSummary.resolution, 0, 'g', 6)
            .arg(m_mapSummary.normalPointCount)
            .arg(m_mapSummary.stationCount)
            .arg(m_mapSummary.pathCount)
            .arg(m_mapSummary.areaCount));
}

void MainWindow::chooseAndUpload(bool switchAfterUpload)
{
    if (selectedRows().isEmpty()) {
        QMessageBox::information(this, tx("未选择机器人", "No robot selected"),
                                 tx("请勾选至少一台目标机器人。",
                                    "Select at least one target robot."));
        return;
    }
    if (m_livePositionAction->isChecked()) stopLivePosition();
    const QString fileName = QFileDialog::getOpenFileName(this,
        tx("选择 2D 地图", "Select a 2D map"), {},
        tx("SMAP (*.smap);;JSON (*.json)", "SMAP (*.smap);;JSON (*.json)"));
    if (fileName.isEmpty()) return;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, tx("无法读取地图", "Cannot read map"), file.errorString());
        return;
    }
    const QByteArray bytes = file.readAll();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (document.isNull() || !document.isObject()) {
        QMessageBox::critical(this, tx("地图格式错误", "Invalid map format"),
            tx("2D 地图不是有效的 JSON 对象：%1",
               "The 2D map is not a valid JSON object: %1").arg(parseError.errorString()));
        return;
    }
    const QJsonObject root = document.object();
    const QJsonObject header = root.value(QStringLiteral("header")).toObject();
    QString mapName = header.value(QStringLiteral("mapName")).toString().trimmed();
    if (mapName.isEmpty()) {
        mapName = header.value(QStringLiteral("map_name")).toString().trimmed();
    }
    if (mapName.isEmpty()) {
        mapName = QFileInfo(fileName).completeBaseName().trimmed();
    }
    while (mapName.endsWith(QStringLiteral(".smap"), Qt::CaseInsensitive)) {
        mapName.chop(5);
        mapName = mapName.trimmed();
    }
    if (mapName.isEmpty()) {
        QMessageBox::warning(this, tx("地图名称缺失", "Missing map name"),
            tx("无法从地图内容或文件名中确定地图名称。",
               "The map name could not be determined from the map content or filename."));
        return;
    }
    const QString md5 = QString::fromLatin1(QCryptographicHash::hash(bytes,
        QCryptographicHash::Md5).toHex());
    const QString action = switchAfterUpload ? tx("上传、校验并切换", "Upload, verify and switch")
                                             : tx("仅上传并校验", "Upload and verify only");
    if (QMessageBox::question(this, tx("确认批量操作", "Confirm batch operation"),
        tx("操作：%1\n地图：%2\nMD5：%3\n机器人：%4 台\n\n"
           "运行中、等待中或暂停的机器人将被跳过，不会自动取消任务。",
           "Operation: %1\nMap: %2\nMD5: %3\nRobots: %4\n\n"
           "Running, waiting or suspended robots will be skipped. Tasks will not be canceled automatically.")
            .arg(action, mapName, md5).arg(selectedRows().size())) != QMessageBox::Yes) return;
    startBatch(bytes, switchAfterUpload);
}

void MainWindow::startBatch(const QByteArray &mapBytes, bool switchAfterUpload)
{
    if (m_activeOperations > 0 || !m_pendingRows.isEmpty()) {
        QMessageBox::warning(this, tx("操作进行中", "Operation in progress"),
                             tx("请等待当前批量操作完成。",
                                "Wait for the current batch operation to finish."));
        return;
    }
    m_mapBytes = mapBytes;
    m_mapMd5 = QString::fromLatin1(QCryptographicHash::hash(mapBytes,
                                                            QCryptographicHash::Md5).toHex());
    m_switchAfterUpload = switchAfterUpload;
    const auto rows = selectedRows();
    for (const int row : rows) {
        m_pendingRows.enqueue(row);
        setCell(row, Result, tx("等待预检", "Waiting for precheck"));
    }
    m_totalOperations = rows.size();
    m_completedOperations = 0;
    m_batchLabel->setText(tx("批量操作：0/%1", "Batch operation: 0/%1").arg(m_totalOperations));
    launchNext();
}

void MainWindow::launchNext()
{
    while (m_activeOperations < MaxConcurrentRobots && !m_pendingRows.isEmpty()) {
        const int row = m_pendingRows.dequeue();
        ++m_activeOperations;
        runRobotOperation(row);
    }
    if (m_activeOperations == 0 && m_pendingRows.isEmpty() && m_totalOperations > 0) {
        m_batchLabel->setText(tx("批量操作完成：%1/%1", "Batch operation complete: %1/%1")
                              .arg(m_totalOperations));
        m_mapBytes.clear();
        m_totalOperations = 0;
    }
}

void MainWindow::runRobotOperation(int row)
{
    auto *deployer = new MapDeployer(this);
    connect(deployer, &MapDeployer::progress, this, [this, row](QString chinese, QString english) {
        setCell(row, Result, m_english ? english : chinese);
    });
    connect(deployer, &MapDeployer::navigationStatus, this, [this, row](int status) {
        setCell(row, Task, statusName(status, m_english));
        m_table->item(row, Task)->setData(Qt::UserRole, status);
    });
    connect(deployer, &MapDeployer::currentMap, this, [this, row](QString name, QString md5) {
        setCell(row, CurrentMap, name);
        setCell(row, Md5, md5);
    });
    connect(deployer, &MapDeployer::finished, this, [this, row, deployer](DeployResult result) {
        QString message = result.message;
        if (result.result == "done") {
            if (result.message == "Current map already matches the requested MD5") {
                message = tx("当前地图 MD5 已匹配", "Current map already matches the requested MD5");
            } else message = m_switchAfterUpload
                ? tx("上传、校验和切换成功", "Upload, verification and switch succeeded")
                : tx("上传及 MD5 校验成功", "Upload and MD5 verification succeeded");
        } else if (result.result == "busy") {
            message = tx("已跳过：机器人忙", "Skipped: robot is busy");
        } else {
            message = tx("操作失败：%1", "Operation failed: %1").arg(localizedError(message));
        }
        finishRobot(row, message, result.result == "done");
        deployer->deleteLater();
    });
    deployer->deploy(robotAt(row), m_mapBytes, m_mapMd5, m_switchAfterUpload);
}

void MainWindow::finishRobot(int row, const QString &result, bool success)
{
    setCell(row, Result, result);
    log(QStringLiteral("%1 (%2): %3").arg(robotAt(row).name, robotAt(row).host, result));
    auto *item = m_table->item(row, Result);
    item->setForeground(success ? QColor(0, 128, 50) : QColor(180, 50, 40));
    --m_activeOperations;
    ++m_completedOperations;
    m_batchLabel->setText(tx("批量操作：%1/%2", "Batch operation: %1/%2")
                          .arg(m_completedOperations).arg(m_totalOperations));
    launchNext();
}

void MainWindow::query(int row, quint16 port, quint16 command, const QByteArray &payload,
                       std::function<void(RequestResult)> callback, int timeoutMs)
{
    const Robot robot = robotAt(row);
    RequestJob::start(this, robot.host, port, command, payload, std::move(callback), timeoutMs);
}

Robot MainWindow::robotAt(int row) const
{
    Robot robot;
    robot.name = m_table->item(row, Name)->text();
    robot.host = m_table->item(row, Ip)->text();
    return robot;
}

QList<int> MainWindow::selectedRows() const
{
    QList<int> result;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *box = m_table->cellWidget(row, Selected);
        auto *check = box ? box->findChild<QCheckBox *>() : nullptr;
        if (check && check->isChecked()) result.append(row);
    }
    return result;
}

void MainWindow::setCell(int row, Column column, const QString &text)
{
    auto *item = m_table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem;
        if (column != Selected) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, column, item);
    }
    item->setText(text);
    if (column == Result) item->setData(Qt::UserRole, QVariant());
    item->setForeground(palette().color(QPalette::Text));
}

void MainWindow::log(const QString &message)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), message));
}

bool MainWindow::parseJson(const QByteArray &bytes, QJsonObject *object, QString *error) const
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (!document.isObject()) {
        if (error) *error = tx("JSON 响应无效：%1", "Invalid JSON response: %1")
                                .arg(parseError.errorString());
        return false;
    }
    *object = document.object();
    return true;
}

bool MainWindow::responseSucceeded(const QJsonObject &object, QString *error) const
{
    const int code = object.value(QStringLiteral("ret_code")).toInt(0);
    if (code == 0) return true;
    if (error) *error = tx("错误 %1：%2", "Error %1: %2").arg(code)
        .arg(object.value(QStringLiteral("err_msg")).toString(tx("未知错误", "Unknown error")));
    return false;
}
