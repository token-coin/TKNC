#ifndef TKN_QT_MININGPAGE_H
#define TKN_QT_MININGPAGE_H

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

#include <miner/mode.h>

class WalletModel;
class ClientModel;
class PlatformStyle;

class MiningPage : public QWidget {
    Q_OBJECT

private:
    QPushButton* selectModelButton;
    QLabel* modelPathLabel;
    QLabel* modelStatusLabel;
    QProgressBar* modelLoadProgress;
    
    QLabel* modeLabel;
    QLabel* activeRequestsLabel;
    QLabel* hashrateLabel;
    QLabel* blockHeightLabel;
    QLabel* difficultyLabel;
    
    QGroupBox* gpuResourceGroup;
    QLabel* gpuMemoryLabel;
    QLabel* gpuUtilizationLabel;
    QLabel* maxConcurrentLabel;
    QProgressBar* gpuMemoryBar;
    
    QLabel* todayEarningsLabel;
    QLabel* weekEarningsLabel;
    QLabel* monthEarningsLabel;
    QLabel* totalEarningsLabel;
    
    QPushButton* startButton;
    QPushButton* stopButton;
    QPushButton* switchModeButton;
    
    QVBoxLayout* mainLayout;
    QGroupBox* statusGroup;
    QGroupBox* modelGroup;
    QGroupBox* earningsGroup;
    QGroupBox* controlGroup;
    
    bool isMining;
    MiningMode currentMode;
    QString currentModelPath;
    bool isModelLoaded;

    WalletModel* walletModel{nullptr};
    ClientModel* clientModel{nullptr};
    QTimer* updateTimer{nullptr};

public:
    explicit MiningPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* model);

private Q_SLOTS:
    void OnSelectModel();
    
    void UpdateMiningStatus();
    
    void UpdateEarnings();
    
    void StartMining();
    
    void StopMining();
    
    void OnSwitchMode();
    
    void SwitchToTaskMode();
    
    void SwitchToPowMode();
    
    void StopCurrentMode();
    
    void UpdateUIForMode();

private:
    void SetupUI();
    
    void ConnectSignals();
};

#endif // TKN_QT_MININGPAGE_H
