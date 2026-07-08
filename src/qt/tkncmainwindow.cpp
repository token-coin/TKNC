#include <span.h>
#include <serialize.h>

#include <qt/nabmainwindow.h>  // Filename kept for build system compatibility
#include <qt/walletpage.h>
#include <qt/miningpage.h>
#include <qt/modelpage.h>
#include <qt/reviewpage.h>
#include <qt/resourcepage.h>
#include <qt/apikeypage.h>  // [New]
#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <qt/clientmodel.h>

#include <interfaces/wallet.h>
#include <util/log.h>

#include <QApplication>
#include <QStyle>
#include <QDebug>

TKNCMainWindow::TKNCMainWindow(const PlatformStyle* _platformStyle, QWidget* parent) 
    : QMainWindow(parent)
    , platformStyle(_platformStyle)
{
    SetupUI();
    ConnectSignals();
    UpdateStatusBar();

    qDebug() << "TKNCMainWindow: Initializing finished, waiting for WalletModel and ClientModel connection...";
}

void TKNCMainWindow::SetupUI() {
    setWindowTitle(tr("TKNC-Qt - TokenCoin AI Computing Power Platform"));
    resize(1200, 800);

    mainMenuBar = this->menuBar();
    fileMenu = mainMenuBar->addMenu(tr("File"));
    editMenu = mainMenuBar->addMenu(tr("Edit"));
    viewMenu = mainMenuBar->addMenu(tr("View"));
    toolsMenu = mainMenuBar->addMenu(tr("Tools"));
    helpMenu = mainMenuBar->addMenu(tr("Help"));
    
    fileMenu->addAction(tr("Quit"), qApp, &QApplication::quit);
    
    toolBar = addToolBar(tr("Toolbar"));
    walletAction = toolBar->addAction(tr("Wallet"));
    miningAction = toolBar->addAction(tr("Mining"));
    modelAction = toolBar->addAction(tr("Models"));
    reviewAction = toolBar->addAction(tr("Review"));
    resourceAction = toolBar->addAction(tr("Resources"));
    settingsAction = toolBar->addAction(tr("Settings"));
    
    sideBar = new QListWidget(this);
    sideBar->addItem(tr("Wallet"));
    sideBar->addItem(tr("Mining"));
    sideBar->addItem(tr("Models"));
    sideBar->addItem(tr("Review"));
    sideBar->addItem(tr("Resources"));
    sideBar->addItem(tr("API Key"));
    sideBar->setMaximumWidth(150);
    sideBar->setCurrentRow(0);
    
    contentArea = new QStackedWidget(this);
    walletPage = new WalletPage(platformStyle, this);  // [Modified]pass PlatformStyle
    miningPage = new MiningPage(this);
    modelPage = new ModelPage(this);
    reviewPage = new ReviewPage(this);
    resourcePage = new ResourcePage(this);  // [New]CreateResourcePage
    apiKeyPage = new APIKeyPage(platformStyle, this);  // [New]CreateAPIKeyPage

    contentArea->addWidget(walletPage);
    contentArea->addWidget(miningPage);
    contentArea->addWidget(modelPage);
    contentArea->addWidget(reviewPage);
    contentArea->addWidget(resourcePage);  // [New]add to content area
    contentArea->addWidget(apiKeyPage);   // [New]add to content area
    
    QWidget* centralWidget = new QWidget(this);
    QHBoxLayout* mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->addWidget(sideBar);
    mainLayout->addWidget(contentArea);
    mainLayout->setStretch(1, 1);
    setCentralWidget(centralWidget);
    
    mainStatusBar = this->statusBar();
    connectionCountLabel = new QLabel(tr("Connections: 0"), mainStatusBar);
    blockHeightLabel = new QLabel(tr("Height: 0"), mainStatusBar);
    currentModeLabel = new QLabel(tr("Mode: PoW"), mainStatusBar);
    balanceLabel = new QLabel(tr("Balance: 0 TKNC"), mainStatusBar);

    mainStatusBar->addWidget(connectionCountLabel);
    mainStatusBar->addWidget(blockHeightLabel);
    mainStatusBar->addWidget(currentModeLabel);
    mainStatusBar->addWidget(balanceLabel);
}

void TKNCMainWindow::ConnectSignals() {
    connect(walletAction, &QAction::triggered, this, &TKNCMainWindow::SwitchToWallet);
    connect(miningAction, &QAction::triggered, this, &TKNCMainWindow::SwitchToMining);
    connect(modelAction, &QAction::triggered, this, &TKNCMainWindow::SwitchToModel);
    connect(reviewAction, &QAction::triggered, this, &TKNCMainWindow::SwitchToReview);
    connect(resourceAction, &QAction::triggered, this, &TKNCMainWindow::SwitchToResource);  // [New]resource button connection
    
    connect(sideBar, &QListWidget::currentRowChanged, [this](int row) {
        contentArea->setCurrentIndex(row);
    });
}

void TKNCMainWindow::SwitchToWallet() {
    sideBar->setCurrentRow(0);
    contentArea->setCurrentIndex(0);
}

void TKNCMainWindow::SwitchToMining() {
    sideBar->setCurrentRow(1);
    contentArea->setCurrentIndex(1);
}

void TKNCMainWindow::SwitchToModel() {
    sideBar->setCurrentRow(2);
    contentArea->setCurrentIndex(2);
}

void TKNCMainWindow::SwitchToReview() {
    sideBar->setCurrentRow(3);
    contentArea->setCurrentIndex(3);
}

void TKNCMainWindow::SwitchToResource() {  // [New]switch to resource page
    sideBar->setCurrentRow(4);
    contentArea->setCurrentIndex(4);
}

void TKNCMainWindow::SwitchToAPIKey() {  // [New]switch to API Key page
    sideBar->setCurrentRow(5);
    contentArea->setCurrentIndex(5);
}

void TKNCMainWindow::UpdateStatusBar() {
    connectionCountLabel->setText(tr("Connections: 0"));
    blockHeightLabel->setText(tr("Height: 0"));
    currentModeLabel->setText(tr("Mode: PoW"));
    balanceLabel->setText(tr("Balance: 0 TKNC"));
}

void TKNCMainWindow::UpdateConnectionCount(int count) {
    connectionCountLabel->setText(tr("Connections: %1").arg(count));
}

void TKNCMainWindow::UpdateBlockHeight(int height) {
    blockHeightLabel->setText(tr("Height: %1").arg(height));
}

void TKNCMainWindow::UpdateCurrentMode(const QString& mode) {
    currentModeLabel->setText(tr("Mode: %1").arg(mode));
}

void TKNCMainWindow::UpdateBalance(double balance) {
    balanceLabel->setText(tr("Balance: %1 TKNC").arg(balance, 0, 'f', 2));
}

void TKNCMainWindow::setWalletModel(WalletModel* model)
{
    walletModel = model;
    
    if (!walletModel) {
        qWarning() << "TKNCMainWindow: WalletModel is null";
        return;
    }

    qDebug() << "TKNCMainWindow: WalletModel connected, passing to pages...";
    
    // Pass WalletModel to all pages that need it
    walletPage->setWalletModel(walletModel);
    miningPage->setWalletModel(walletModel);  // [New]pass to MiningPage
    apiKeyPage->setWalletModel(walletModel);  // pass to APIKeyPage

    // Connect balance change signal to status bar
    connect(walletModel, &WalletModel::balanceChanged, [this](const interfaces::WalletBalances& balances) {
        // Update status bar balance display
        double totalBalance = (balances.balance + balances.unconfirmed_balance + balances.immature_balance) / 100000000.0;
        UpdateBalance(totalBalance);
        
        qDebug() << "TKNCMainWindow: Status bar balance update to" << totalBalance << "TKNC";
    });

    // Initialize status bar display
    const auto& balances = walletModel->getCachedBalance();
    double initialBalance = (balances.balance + balances.unconfirmed_balance + balances.immature_balance) / 100000000.0;
    UpdateBalance(initialBalance);
}

void TKNCMainWindow::setClientModel(ClientModel* _clientModel)
{
    clientModel = _clientModel;
    
    if (!clientModel) {
        qWarning() << "TKNCMainWindow: ClientModel is null";
        return;
    }

    qDebug() << "TKNCMainWindow: ClientModel connected, passing to pages...";

    // Pass ClientModel to all pages that need it
    walletPage->setClientModel(clientModel);
    miningPage->setClientModel(clientModel);  // [New]pass to MiningPage (critical)
    resourcePage->setClientModel(clientModel);  // [New]pass to ResourcePage
    apiKeyPage->setClientModel(clientModel);  // [New]pass to APIKeyPage (required for RPC)

    // Connect node count change signal to status bar
    connect(clientModel, &ClientModel::numConnectionsChanged, this, &TKNCMainWindow::UpdateConnectionCount);
    
    // Connect block height change signal to status bar
    connect(clientModel, &ClientModel::numBlocksChanged, this, &TKNCMainWindow::UpdateBlockHeight);

    // Initialize status bar display
    UpdateConnectionCount(clientModel->getNumConnections());
    UpdateBlockHeight(clientModel->getNumBlocks());

    qDebug() << "TKNCMainWindow: ClientModel integration finished, connections:" 
             << clientModel->getNumConnections() 
             << "Block Height:" 
             << clientModel->getNumBlocks();
}
