#ifndef TKN_QT_RESOURCEPAGE_H
#define TKN_QT_RESOURCEPAGE_H

#include <tknc-build-config.h>
#include <QObject>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTimer>

class ClientModel;

class ResourcePage : public QWidget {
    Q_OBJECT

private:
    QGroupBox* systemOverviewGroup;
    QLabel* cpuUsageLabel;
    QLabel* memoryUsageLabel;
    QLabel* gpuUsageLabel;
    QLabel* networkSpeedLabel;
    QProgressBar* cpuProgressBar;
    QProgressBar* memoryProgressBar;
    QProgressBar* gpuProgressBar;
    
    QGroupBox* gpuInfoGroup;
    QLabel* gpuModelLabel;
    QLabel* totalMemoryLabel;
    QLabel* usedMemoryLabel;
    QLabel* availableMemoryLabel;
    QLabel* modelOccupiedLabel;
    QLabel* requestBufferLabel;
    QLabel* systemReservedLabel;
    
    QGroupBox* apiStatusGroup;
    QLabel* apiPortLabel;
    QLabel* apiStatusLabel;
    QLabel* totalRequestsLabel;
    QLabel* todayRequestsLabel;
    QLabel* todayEarningsLabel;
    QTableWidget* activeRequestsTable;
    
    QGroupBox* p2pStatusGroup;
    QLabel* p2pPortLabel;
    QLabel* p2pConnectionsLabel;
    QLabel* p2pHeightLabel;
    QLabel* p2pDifficultyLabel;
    
    QVBoxLayout* mainLayout;
    
    QTimer* updateTimer;

    ClientModel* clientModel{nullptr};

public:
    explicit ResourcePage(QWidget* parent = nullptr);
    ~ResourcePage();

    void setClientModel(ClientModel* model);

private Q_SLOTS:
    void UpdateAllResourceInfo();
    
    void UpdateSystemOverview();
    
    void UpdateGPUInfo();
    
    void UpdateAPIStatus();
    
    void UpdateP2PStatus();
};

#endif // TKN_QT_RESOURCEPAGE_H
