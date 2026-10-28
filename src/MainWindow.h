#pragma once

#include "core/Robot.h"
#include "map/MapView.h"
#include "network/RequestJob.h"

#include <QMainWindow>
#include <QHash>
#include <QQueue>
#include <QStringList>
#include <QVector>

class QLabel;
class QPlainTextEdit;
class QTableWidget;
class QJsonObject;
class QAction;
class QComboBox;
class QToolBar;
class QTabWidget;
class QTimer;
class QPushButton;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    enum Column { Selected, Name, Ip, Online, Task, CurrentMap, Md5, Result, ColumnCount };
    enum SampleAnomaly {
        LowConfidence = 0x01,
        ConfidenceDrop = 0x02,
        PositionJump = 0x04,
        OutsideMap = 0x08,
        MissingConfidence = 0x10
    };

    struct LocalizationSample {
        qint64 timestampMs = 0;
        double x = 0.0;
        double y = 0.0;
        double angle = 0.0;
        double confidence = -1.0;
        double distance = 0.0;
        double speed = 0.0;
        double confidenceDelta = 0.0;
        int method = -1;
        int anomalies = 0;
        bool hasPrevious = false;
        bool hasConfidenceDelta = false;
    };

    void buildUi();
    void retranslateUi();
    QString tx(const char *chinese, const char *english) const;
    QString localizedError(const QString &error) const;
    void loadRobots();
    void saveRobots() const;
    void addRobot();
    void removeSelected();
    void refreshSelected();
    void startLivePosition();
    void stopLivePosition(bool clearMarker = true);
    void pollLivePosition();
    void recordLocalizationSample(const Robot &robot, double x, double y,
                                  double angle, double confidence, int method);
    void clearSamplingData();
    void exportSamplingCsv();
    void importHeatmapCsv();
    void clearHeatmapData();
    void updateHeatmapSummary();
    void updateSamplingSummary();
    QString sampleAnomalyText(int anomalies) const;
    void downloadMap();
    void resolveDownloadMap(int row);
    void findDownloadMapByMd5(int row, const QStringList &storedFiles,
                              const QString &expectedMd5, int fallbackIndex = -1);
    void downloadStoredMap(int row, const QString &storedFileName);
    void openMapFile();
    void showMapWindow(bool fit = false);
    QWidget *mapDialogParent();
    void updateMapSummary();
    void chooseAndUpload(bool switchAfterUpload);
    void startBatch(const QByteArray &mapBytes, bool switchAfterUpload);
    void launchNext();
    void runRobotOperation(int row);
    void finishRobot(int row, const QString &result, bool success);
    void query(int row, quint16 port, quint16 command, const QByteArray &payload,
               std::function<void(RequestResult)> callback, int timeoutMs = 10000);
    Robot robotAt(int row) const;
    QList<int> selectedRows() const;
    void setCell(int row, Column column, const QString &text);
    void log(const QString &message);
    bool parseJson(const QByteArray &bytes, QJsonObject *object, QString *error) const;
    bool responseSucceeded(const QJsonObject &object, QString *error) const;

    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel *m_batchLabel = nullptr;
    QLabel *m_logoLabel = nullptr;
    QLabel *m_languageLabel = nullptr;
    QLabel *m_mapInfoLabel = nullptr;
    QToolBar *m_toolbar = nullptr;
    QTabWidget *m_tabs = nullptr;
    QWidget *m_mapWindow = nullptr;
    MapView *m_mapView = nullptr;
    QComboBox *m_languageCombo = nullptr;
    QAction *m_addAction = nullptr;
    QAction *m_removeAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_downloadAction = nullptr;
    QAction *m_openMapAction = nullptr;
    QAction *m_showMapAction = nullptr;
    QAction *m_livePositionAction = nullptr;
    QAction *m_clearTrackAction = nullptr;
    QAction *m_exportSamplesAction = nullptr;
    QAction *m_uploadAction = nullptr;
    QAction *m_uploadSwitchAction = nullptr;
    QTimer *m_locationTimer = nullptr;
    QLabel *m_livePositionLabel = nullptr;
    QLabel *m_heatmapSummaryLabel = nullptr;
    QLabel *m_samplingSummaryLabel = nullptr;
    QTableWidget *m_samplingTable = nullptr;
    QPushButton *m_importHeatmapButton = nullptr;
    QPushButton *m_toggleHeatmapButton = nullptr;
    QPushButton *m_clearHeatmapButton = nullptr;
    QPushButton *m_fitMapButton = nullptr;
    QPushButton *m_closeMapButton = nullptr;
    QQueue<int> m_pendingRows;
    int m_activeOperations = 0;
    int m_completedOperations = 0;
    int m_totalOperations = 0;
    QByteArray m_mapBytes;
    QString m_mapMd5;
    int m_livePositionRow = -1;
    int m_livePositionSession = 0;
    int m_livePositionFailures = 0;
    bool m_locationRequestPending = false;
    QVector<LocalizationSample> m_localizationSamples;
    QString m_samplingRobotName;
    QString m_samplingRobotHost;
    QString m_samplingMapName;
    double m_confidenceSum = 0.0;
    double m_minConfidence = 1.0;
    int m_validConfidenceSamples = 0;
    int m_anomalySamples = 0;
    int m_failedLocationReads = 0;
    QVector<ConfidenceHeatmapCell> m_heatmapCells;
    int m_heatmapRowCount = 0;
    int m_heatmapMovingSamples = 0;
    int m_heatmapRejectedSamples = 0;
    int m_heatmapFiles = 0;
    QString m_heatmapRobotName;
    int m_heatmapImportSession = 0;
    bool m_switchAfterUpload = false;
    bool m_english = false;
    bool m_hasMapSummary = false;
    MapSummary m_mapSummary;
};
