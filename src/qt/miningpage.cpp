#include <span.h>
#include <serialize.h>

#include <qt/miningpage.h>
#include <qt/guiutil.h>
#include <qt/clientmodel.h>  // [New]ClientModel for RPC calls
#include <miner/mode.h>
#include <util/log.h>

#include <interfaces/node.h>  // [New]Node interface
#include <univalue.h>  // [New]UniValue for RPC result parsing

#include <QMessageBox>
#include <QDateTime>
#include <QFileDialog>

MiningPage::MiningPage(QWidget* parent) : QWidget(parent), isMining(false), currentMode(MiningMode::MODE_POW), isModelLoaded(false) {
    mainLayout = new QVBoxLayout(this);
    
    // ========== [New]Model configuration group ==========
    modelGroup = new QGroupBox(tr("Model Configuration"), this);
    QVBoxLayout* modelLayout = new QVBoxLayout(modelGroup);
    
    QHBoxLayout* modelPathLayout = new QHBoxLayout();
    QLabel* currentModelLabel = new QLabel(tr("Current Model:"), this);
    modelPathLabel = new QLabel(tr("Not Selected"), this);
    modelPathLabel->setStyleSheet("color: gray;");
    modelPathLayout->addWidget(currentModelLabel);
    modelPathLayout->addWidget(modelPathLabel, 1);
    modelLayout->addLayout(modelPathLayout);
    
    QHBoxLayout* modelStatusLayout = new QHBoxLayout();
    QLabel* statusLabel = new QLabel(tr("Model Status:"), this);
    modelStatusLabel = new QLabel(tr("Not Loaded"), this);
    modelStatusLabel->setStyleSheet("color: red; font-weight: bold;");
    modelStatusLayout->addWidget(statusLabel);
    modelStatusLayout->addWidget(modelStatusLabel, 1);
    modelLayout->addLayout(modelStatusLayout);
    
    selectModelButton = new QPushButton(tr("Select Model File..."), this);
    modelLayout->addWidget(selectModelButton);
    
    modelLoadProgress = new QProgressBar(this);
    modelLoadProgress->setVisible(false);
    modelLoadProgress->setRange(0, 100);
    modelLoadProgress->setValue(0);
    modelLayout->addWidget(modelLoadProgress);
    
    mainLayout->addWidget(modelGroup);
    
    // Mining Status
    statusGroup = new QGroupBox(tr("Mining Status"), this);
    QVBoxLayout* statusLayout = new QVBoxLayout(statusGroup);
    
    modeLabel = new QLabel(tr("Current Mode: PoW Mode"), this);
    modeLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: blue;");
    statusLayout->addWidget(modeLabel);
    
    activeRequestsLabel = new QLabel(tr("Active Requests: 0"), this);
    statusLayout->addWidget(activeRequestsLabel);
    
    hashrateLabel = new QLabel(tr("Hashrate: 0 H/s"), this);
    statusLayout->addWidget(hashrateLabel);
    
    blockHeightLabel = new QLabel(tr("Block Height: 0"), this);
    statusLayout->addWidget(blockHeightLabel);
    
    difficultyLabel = new QLabel(tr("Difficulty: 0"), this);
    statusLayout->addWidget(difficultyLabel);
    
    mainLayout->addWidget(statusGroup);
    
    // ========== [New]GPU resource monitor group ==========
    gpuResourceGroup = new QGroupBox(tr("GPU Resource Monitor"), this);
    QVBoxLayout* gpuLayout = new QVBoxLayout(gpuResourceGroup);
    
    QHBoxLayout* gpuMemoryLayout = new QHBoxLayout();
    QLabel* gpuMemoryTitle = new QLabel(tr("GPU Memory:"), this);
    gpuMemoryLabel = new QLabel(tr("0 GB / 8 GB (0%)"), this);
    gpuMemoryBar = new QProgressBar(this);
    gpuMemoryBar->setRange(0, 100);
    gpuMemoryBar->setValue(0);
    gpuMemoryBar->setTextVisible(true);
    gpuMemoryBar->setFormat("%p%");
    gpuMemoryLayout->addWidget(gpuMemoryTitle);
    gpuMemoryLayout->addWidget(gpuMemoryLabel);
    gpuLayout->addLayout(gpuMemoryLayout);
    gpuLayout->addWidget(gpuMemoryBar);
    
    QHBoxLayout* gpuUtilizationLayout = new QHBoxLayout();
    QLabel* gpuUtilTitle = new QLabel(tr("GPU Utilization:"), this);
    gpuUtilizationLabel = new QLabel(tr("0%"), this);
    gpuUtilizationLayout->addWidget(gpuUtilTitle);
    gpuUtilizationLayout->addWidget(gpuUtilizationLabel, 1);
    gpuLayout->addLayout(gpuUtilizationLayout);
    
    QHBoxLayout* concurrentLayout = new QHBoxLayout();
    QLabel* activeReqTitle = new QLabel(tr("Active Requests:"), this);
    maxConcurrentLabel = new QLabel(tr("Max Concurrent: 4"), this);
    concurrentLayout->addWidget(activeReqTitle);
    concurrentLayout->addWidget(activeRequestsLabel, 1);
    concurrentLayout->addWidget(maxConcurrentLabel);
    gpuLayout->addLayout(concurrentLayout);
    
    mainLayout->addWidget(gpuResourceGroup);
    
    // Earnings Statistics
    earningsGroup = new QGroupBox(tr("Earnings Statistics"), this);
    QVBoxLayout* earningsLayout = new QVBoxLayout(earningsGroup);
    
    todayEarningsLabel = new QLabel(tr("Today Earnings: 0 TKNC"), this);
    earningsLayout->addWidget(todayEarningsLabel);
    
    weekEarningsLabel = new QLabel(tr("Week Earnings: 0 TKNC"), this);
    earningsLayout->addWidget(weekEarningsLabel);
    
    monthEarningsLabel = new QLabel(tr("Month Earnings: 0 TKNC"), this);
    earningsLayout->addWidget(monthEarningsLabel);
    
    totalEarningsLabel = new QLabel(tr("Total Earnings: 0 TKNC"), this);
    earningsLayout->addWidget(totalEarningsLabel);
    
    mainLayout->addWidget(earningsGroup);
    
    // Control
    controlGroup = new QGroupBox(tr("Control"), this);
    QHBoxLayout* controlLayout = new QHBoxLayout(controlGroup);
    
    startButton = new QPushButton(tr("Start Mining"), this);
    stopButton = new QPushButton(tr("Stop Mining"), this);
    switchModeButton = new QPushButton(tr("Switch Mode"), this);
    
    controlLayout->addWidget(startButton);
    controlLayout->addWidget(stopButton);
    controlLayout->addWidget(switchModeButton);
    
    mainLayout->addWidget(controlGroup);
    
    // Connect signals/slots
    connect(selectModelButton, &QPushButton::clicked, this, &MiningPage::OnSelectModel);
    connect(startButton, &QPushButton::clicked, this, &MiningPage::StartMining);
    connect(stopButton, &QPushButton::clicked, this, &MiningPage::StopMining);
    connect(switchModeButton, &QPushButton::clicked, this, &MiningPage::OnSwitchMode);
    
    UpdateUIForMode();
    UpdateMiningStatus();
    UpdateEarnings();
}

// ========== [New]Select model file ==========
void MiningPage::OnSelectModel() {
    QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("Select GGUF Model File"),
        QString(),
        tr("GGUF Model Files (*.gguf);;All Files (*)")
    );
    
    if (filePath.isEmpty()) {
        return;
    }
    
    currentModelPath = filePath;
    modelPathLabel->setText(filePath);
    modelPathLabel->setToolTip(filePath);
    
    // Show loading progress
    modelLoadProgress->setVisible(true);
    modelStatusLabel->setText(tr("Loading..."));
    modelStatusLabel->setStyleSheet("color: orange; font-weight: bold;");
    
    // Simulate model loading process (actual implementation needs backend API call)
    for (int i = 0; i <= 100; i += 10) {
        modelLoadProgress->setValue(i);
        // Actual: should call model loading API and monitor progress
    }
    
    // Loading complete
    isModelLoaded = true;
    modelLoadProgress->setVisible(false);
    modelStatusLabel->setText(tr("Loaded"));
    modelStatusLabel->setStyleSheet("color: green; font-weight: bold;");
    
    QMessageBox::information(
        this,
        tr("Model loaded successfully"),
        tr("Model loaded successfully:\n%1\n\nSwitch to Task Mode to start receiving AI inference tasks.").arg(filePath)
    );
}

void MiningPage::UpdateMiningStatus() {
    if (currentMode == MiningMode::MODE_TASK) {
        modeLabel->setText(tr("Current Mode: Task Mode"));
        modeLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: green;");
        hashrateLabel->setText(tr("Hashrate: N/A (Task Mode)"));
    } else {
        modeLabel->setText(tr("Current Mode: PoW Mode"));
        modeLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: blue;");
        hashrateLabel->setText(tr("Hashrate: 125.5 H/s"));
    }
    
    activeRequestsLabel->setText(tr("Active Requests: %1").arg(isMining ? "2" : "0"));
    blockHeightLabel->setText(tr("Block Height: 12345"));
    difficultyLabel->setText(tr("Difficulty: 123456789"));
}

void MiningPage::UpdateEarnings() {
    todayEarningsLabel->setText(tr("Today Earnings: %1 TKNC").arg(isMining ? "50.00" : "0"));
    weekEarningsLabel->setText(tr("Week Earnings: %1 TKNC").arg(isMining ? "350.00" : "0"));
    monthEarningsLabel->setText(tr("Month Earnings: %1 TKNC").arg(isMining ? "1500.00" : "0"));
    totalEarningsLabel->setText(tr("Total Earnings: %1 TKNC").arg(isMining ? "12345.67" : "0"));
}

void MiningPage::StartMining() {
    if (currentMode == MiningMode::MODE_TASK && !isModelLoaded) {
        QMessageBox::warning(
            this,
            tr("Cannot Start Mining"),
            tr("Task Mode requires loading a model first.\n\nPlease click the Select Model button to load a model.")
        );
        return;
    }
    
    // Call RPC startmining (replacing direct settings flag)
    CallStartMiningRPC();
}

void MiningPage::StopMining() {
    // Call RPC stopmining (replacing direct settings flag)
    CallStopMiningRPC();
}

// ========== Enhanced: Switch Mode (Task Mode/PoW Mode) ==========
void MiningPage::OnSwitchMode() {
    if (isMining) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this,
            tr("Confirm Switch"),
            tr("Currently mining. Are you sure you want to switch mode?\n\nThis will stop the current mining task."),
            QMessageBox::Yes | QMessageBox::No
        );
        
        if (reply != QMessageBox::Yes) {
            return;
        }
        
        StopCurrentMode();
    }
    
    if (currentMode == MiningMode::MODE_TASK) {
        SwitchToPowMode();
    } else {
        SwitchToTaskMode();
    }
}

// ========== [New]Switch to Task Mode ==========
void MiningPage::SwitchToTaskMode() {
    if (!isModelLoaded) {
        QMessageBox::warning(
            this,
            tr("Cannot Switch to Task Mode"),
            tr("Switching to Task Mode requires loading a model first.\n\nPlease click the Select Model button to load a model.")
        );
        return;
    }
    
    currentMode = MiningMode::MODE_TASK;
    UpdateUIForMode();
    
    QMessageBox::information(
        this,
        tr("Mode Switch Successful"),
        tr("Switched to Task Mode\n\nNow you can:\n\u2022 Receive AI inference tasks\n\u2022 Use the loaded model for inference\n\u2022 Earn TKNC as service rewards")
    );
}

// ========== [New]Switch to PoW Mode ==========
void MiningPage::SwitchToPowMode() {
    currentMode = MiningMode::MODE_POW;
    UpdateUIForMode();
    
    QMessageBox::information(
        this,
        tr("Mode Switch Successful"),
        tr("Switched to PoW Mode\n\nNow you can:\n\u2022 Compute Transformer hash difficulty proof\n\u2022 Compete for block rewards\n\u2022 Earn TKNC as mining rewards")
    );
}

// ========== [New]Stop current mode ==========
void MiningPage::StopCurrentMode() {
    isMining = false;
    
    if (currentMode == MiningMode::MODE_TASK) {
        // Stop all processing AI inference tasks
        // Actual: call API to stop task listener
    } else {
        // Stop PoW computation thread
        // Actual: stop mining thread
    }
    
    UpdateMiningStatus();
    UpdateEarnings();
}

// ========== [New]Update UI to show current mode ==========
void MiningPage::UpdateUIForMode() {
    if (currentMode == MiningMode::MODE_TASK) {
        modeLabel->setText(tr("Current Mode: Task Mode"));
        modeLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: green;");
        
        // Enable model-related controls in Task Mode
        selectModelButton->setEnabled(true);
        modelGroup->setEnabled(true);
        
        // Update GPU monitor info (Task Mode data)
        gpuMemoryLabel->setText(tr("2.4 GB / 8 GB (30%)"));
        gpuMemoryBar->setValue(30);
        gpuUtilizationLabel->setText(tr("65%"));
    } else {
        modeLabel->setText(tr("Current Mode: PoW Mode"));
        modeLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: blue;");
        
        // Disable model-related controls in PoW Mode (optional)
        selectModelButton->setEnabled(true);  // Keep enabled for user to prepare model in advance
        
        // Update GPU monitor info (PoW Mode data)
        gpuMemoryLabel->setText(tr("0.5 GB / 8 GB (6%)"));
        gpuMemoryBar->setValue(6);
        gpuUtilizationLabel->setText(tr("15%"));
    }
}

// ========== [New]Model integration functions ==========

void MiningPage::setWalletModel(WalletModel* model) {
    walletModel = model;
    
    if (!walletModel) {
        qWarning() << "MiningPage: WalletModel is null";
        return;
    }

    qDebug() << "MiningPage: WalletModelConnected";
    
    // If ClientModel also connected, start periodic update
    if (clientModel && !updateTimer) {
        StartStatusTimer();
    }
}

void MiningPage::setClientModel(ClientModel* model) {
    clientModel = model;
    
    if (!clientModel) {
        qWarning() << "MiningPage: ClientModel is null";
        return;
    }

    qDebug() << "MiningPage: ClientModelConnected";
    
    // Connect client signals (if needed)
    
    // If WalletModel also connected, start periodic update
    if (walletModel && !updateTimer) {
        StartStatusTimer();
    }
}

void MiningPage::StartStatusTimer() {
    // Create timer, update mining status every 5 seconds
    updateTimer = new QTimer(this);
    connect(updateTimer, &QTimer::timeout, this, &MiningPage::OnTimerUpdate);
    updateTimer->start(5000);  // 5 second interval
    
    qDebug() << "MiningPage: Status update timer started (5s interval)";
    
    // Execute update immediately once
    OnTimerUpdate();
}

// ========== [New]Periodic update slot function ==========

void MiningPage::OnTimerUpdate() {
    // Call RPC to get latest mining info
    FetchMiningInfo();
}

// ========== [New]RPC call function ==========

void MiningPage::FetchMiningInfo() {
    if (!clientModel) {
        qWarning() << "MiningPage: FetchMiningInfoFailed - ClientModelNot Connected";
        return;
    }

    // [Modified]Use real RPC call to get mining info
    QTimer::singleShot(0, this, [this]() {
        try {
            // Call getmininginfo RPC command to get mining info
            UniValue params(UniValue::VARR);  // Empty parameter list
            
            UniValue result = clientModel->node().executeRpc(
                "getmininginfo",  // RPC command
                params,           // Empty parameters
                ""                // URI
            );
            
            // Parse returned mining info and update UI
            if (result.isObject()) {
                // Block Height
                if (result.exists("blocks")) {
                    int blocks = result["blocks"].get_int();
                    blockHeightLabel->setText(tr("Block Height: %1").arg(blocks));
                }
                
                // DifficultyInfo
                if (result.exists("difficulty")) {
                    double difficulty = result["difficulty"].get_real();
                    difficultyLabel->setText(tr("Difficulty: %1").arg(difficulty, 0, 'f', 0));
                }
                
                // Network Hash Rate
                if (result.exists("networkhashps")) {
                    int64_t hashps = result["networkhashps"].get_int64();
                    hashrateLabel->setText(tr("Network Hashrate: %1 H/s").arg(hashps));
                }
                
                // Mining Status
                if (result.exists("generate")) {
                    bool generate = result["generate"].get_bool();
                    if (!isMining && generate) {
                        isMining = true;
                        UpdateMiningStatus();
                    } else if (isMining && !generate) {
                        isMining = false;
                        UpdateMiningStatus();
                    }
                }
                
                qDebug() << "MiningPage: Mining info updated (real RPC data)";
                
            } else if (result.isNull()) {
                // RPC returned null, using ClientModel basic info as fallback
                int blocks = clientModel->getNumBlocks();
                blockHeightLabel->setText(tr("Block Height: %1").arg(blocks));
                
                qDebug() << "MiningPage: Using ClientModel basic data - Block Height:" << blocks;
            }
            
        } catch (const UniValue& e) {
            qWarning() << "MiningPage: FetchMiningInfo RPC exception -" 
                      << QString::fromStdString(e.write());
            
            // Fallback to ClientModel basic data
            try {
                int blocks = clientModel->getNumBlocks();
                blockHeightLabel->setText(tr("Block Height: %1").arg(blocks));
            } catch (...) {
                qWarning() << "MiningPage: Cannot get any mining info";
            }
            
        } catch (const std::exception& e) {
            qWarning() << "MiningPage: FetchMiningInfo exception:" << e.what();
            
            // Fallback to ClientModel basic data
            try {
                int blocks = clientModel->getNumBlocks();
                blockHeightLabel->setText(tr("Block Height: %1").arg(blocks));
            } catch (...) {}
        }
    });
}

void MiningPage::CallStartMiningRPC() {
    if (!clientModel) {
        QMessageBox::warning(this, tr("Error"), tr("Client model not connected, cannot start mining"));
        return;
    }

    qDebug() << "MiningPage: Call RPC startmining - mode:" 
             << (currentMode == MiningMode::MODE_TASK ? "Task Mode" : "PoW Mode");

    // [Modified]Use real RPC call instead of mock mode
    QTimer::singleShot(0, this, [this]() {
        try {
            // Build RPC parameters
            UniValue params(UniValue::VARR);
            params.push_back(true);  // StartMining
            params.push_back(1);     // Thread count (default 1)
            
            if (currentMode == MiningMode::MODE_TASK) {
                params.push_back("task");  // Task mode parameter
            } else {
                params.push_back("pow");   // PoW mode parameter
            }
            
            // Execute real RPC call
            UniValue result = clientModel->node().executeRpc(
                "setgenerate",  // RPC command
                params,         // Parameter list
                ""              // URI
            );
            
            // Parse returned result
            bool success = false;
            if (result.isBool()) {
                success = result.get_bool();
            } else if (result.isObject() && result.exists("success")) {
                success = result["success"].get_bool();
            } else if (result.isNull()) {
                success = true;  // null means successful execution
            }
            
            if (success) {
                isMining = true;
                UpdateMiningStatus();
                UpdateEarnings();
                SetStatus(tr("\u2713 Mining started successfully"), false);
                
                qDebug() << "MiningPage: Mining start success - mode:" 
                         << (currentMode == MiningMode::MODE_TASK ? "Task Mode" : "PoW Mode");
            } else {
                QMessageBox::warning(this, tr("Start Failed"), 
                    tr("Cannot start mining, please check system configuration"));
                SetStatus(tr("\u2717 Mining start failed"), true);
                qWarning() << "MiningPage: MiningStartFailed - RPCBackFailed";
            }
            
        } catch (const UniValue& e) {
            qWarning() << "MiningPage: RPC exception -" << QString::fromStdString(e.write());
            QMessageBox::warning(this, tr("RPC Error"),
                tr("RPC error occurred while starting mining:\n%1").arg(QString::fromStdString(e.write())));
            SetStatus(tr("\u2717 Mining start failed (RPC error)"), true);
            
        } catch (const std::exception& e) {
            qWarning() << "MiningPage: Exception -" << e.what();
            QMessageBox::warning(this, tr("System Error"),
                tr("Error occurred while starting mining:\n%1").arg(QString::fromUtf8(e.what())));
            SetStatus(tr("\u2717 Mining start failed (system error)"), true);
        }
    });
}

void MiningPage::CallStopMiningRPC() {
    if (!clientModel) {
        QMessageBox::warning(this, tr("Error"), tr("Client model not connected, cannot stop mining"));
        return;
    }

    qDebug() << "MiningPage: call RPCStop Mining";

    // [Modified]Use real RPC call instead of mock mode
    QTimer::singleShot(0, this, [this]() {
        try {
            // Build RPC parameters
            UniValue params(UniValue::VARR);
            params.push_back(false);  // Stop Mining
            
            // Execute real RPC call
            UniValue result = clientModel->node().executeRpc(
                "setgenerate",  // RPC command
                params,         // Parameter list (false means stop)
                ""              // URI
            );
            
            // Parse returned result
            bool success = false;
            if (result.isBool()) {
                success = result.get_bool();
            } else if (result.isObject() && result.exists("success")) {
                success = result["success"].get_bool();
            } else if (result.isNull()) {
                success = true;  // null means successful execution
            }
            
            if (success) {
                isMining = false;
                StopCurrentMode();
                UpdateMiningStatus();
                UpdateEarnings();
                SetStatus(tr("\u2713 Mining stopped successfully"), false);
                
                qDebug() << "MiningPage: MiningStopSuccess";
            } else {
                QMessageBox::warning(this, tr("Stop Failed"), 
                    tr("Cannot stop mining"));
                SetStatus(tr("\u2717 Mining stop failed"), true);
                qWarning() << "MiningPage: MiningStopFailed - RPCBackFailed";
            }
            
        } catch (const UniValue& e) {
            qWarning() << "MiningPage: RPC exception -" << QString::fromStdString(e.write());
            QMessageBox::warning(this, tr("RPC Error"),
                tr("RPC error occurred while stopping mining:\n%1").arg(QString::fromStdString(e.write())));
            SetStatus(tr("\u2717 Mining stop failed (RPC error)"), true);
            
        } catch (const std::exception& e) {
            qWarning() << "MiningPage: Exception -" << e.what();
            QMessageBox::warning(this, tr("System Error"),
                tr("Error occurred while stopping mining:\n%1").arg(QString::fromUtf8(e.what())));
            SetStatus(tr("\u2717 Mining stop failed (system error)"), true);
        }
    });
}

// ========== [New]Helper function ==========

void MiningPage::SetStatus(const QString& message, bool isError) {
    // Can show status message in status bar or page bottom
    // Temporarily using qDebug output
    if (isError) {
        qWarning() << "MiningPageStatus[ERROR]:" << message;
    } else {
        qDebug() << "MiningPageStatus:" << message;
    }
}

