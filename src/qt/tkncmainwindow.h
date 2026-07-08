#ifndef TKN_QT_TKNCMAINWINDOW_H
#define TKN_QT_TKNCMAINWINDOW_H

#include <tknc-build-config.h>
#include <QObject>
#include <QMainWindow>
#include <QToolBar>
#include <QStatusBar>
#include <QMenuBar>
#include <QAction>
#include <QStackedWidget>
#include <QListWidget>
#include <QLabel>

class WalletPage;
class MiningPage;
class ModelPage;
class ReviewPage;
class ResourcePage;
class APIKeyPage;

class WalletModel;
class ClientModel;
class PlatformStyle;

class TKNCMainWindow : public QMainWindow {
    Q_OBJECT

private:
    QMenuBar* mainMenuBar;
    QMenu* fileMenu;
    QMenu* editMenu;
    QMenu* viewMenu;
    QMenu* toolsMenu;
    QMenu* helpMenu;
    
    QToolBar* toolBar;
    QAction* walletAction;
    QAction* miningAction;
    QAction* modelAction;
    QAction* reviewAction;
    QAction* resourceAction;
    QAction* settingsAction;
    
    QListWidget* sideBar;
    
    QStackedWidget* contentArea;
    WalletPage* walletPage;
    MiningPage* miningPage;
    ModelPage* modelPage;
    ReviewPage* reviewPage;
    ResourcePage* resourcePage;
    APIKeyPage* apiKeyPage;
    
    QStatusBar* mainStatusBar;
    QLabel* connectionCountLabel;
    QLabel* blockHeightLabel;
    QLabel* currentModeLabel;
    QLabel* balanceLabel;

    const PlatformStyle* platformStyle{nullptr};
    WalletModel* walletModel{nullptr};
    ClientModel* clientModel{nullptr};

public:
    explicit TKNCMainWindow(const PlatformStyle* platformStyle, QWidget* parent = nullptr);

    void setWalletModel(WalletModel* model);
    void setClientModel(ClientModel* clientModel);

private Q_SLOTS:
    void SwitchToWallet();
    void SwitchToMining();
    void SwitchToModel();
    void SwitchToReview();
    void SwitchToResource();
    void SwitchToAPIKey();
    
    void UpdateStatusBar();
    
    void UpdateConnectionCount(int count);
    
    void UpdateBlockHeight(int height);
    
    void UpdateCurrentMode(const QString& mode);
    
    void UpdateBalance(double balance);
    
    void SetupUI();
    
    void ConnectSignals();
};

#endif // TKN_QT_TKNCMAINWINDOW_H
