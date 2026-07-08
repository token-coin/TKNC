#include <span.h>
#include <serialize.h>

#include <qt/modelpage.h>
#include <qt/guiutil.h>
#include <util/log.h>

#include <QMessageBox>
#include <QDateTime>
#include <QFileDialog>  // [New]File/directory dialog
#include <QDir>  // [New]Directory action
#include <QFileInfo>  // [New]File info
#include <QSettings>  // [New]Config file read/write

ModelPage::ModelPage(QWidget* parent) : QWidget(parent) {
    mainLayout = new QVBoxLayout(this);
    
    browseGroup = new QGroupBox(tr("Model Browser"), this);
    QVBoxLayout* browseLayout = new QVBoxLayout(browseGroup);
    
    QHBoxLayout* filterLayout = new QHBoxLayout();
    
    // [New]Browse directory button
    browseDirButton = new QPushButton(tr("\ud83d\udcc1 Browse Model Directory"), this);
    browseDirButton->setToolTip(tr("Select a local directory containing AI models"));
    filterLayout->addWidget(browseDirButton);
    
    filterLayout->addWidget(new QLabel(tr("Type:"), this));
    modelTypeFilter = new QComboBox(this);
    modelTypeFilter->addItems({"All", "text", "image", "video", "audio", "code"});
    filterLayout->addWidget(modelTypeFilter);
    
    filterLayout->addWidget(new QLabel(tr("Search:"), this));
    searchBox = new QLineEdit(this);
    searchBox->setPlaceholderText(tr("Enter model name..."));
    filterLayout->addWidget(searchBox);
    
    browseLayout->addLayout(filterLayout);
    
    modelList = new QListWidget(this);
    modelList->addItem("qwen3-72b (text)");
    modelList->addItem("llama3-70b (text)");
    modelList->addItem("stable-diffusion-xl (image)");
    modelList->addItem("stable-video-diffusion (video)");
    modelList->addItem("stable-audio-2.0 (audio)");
    browseLayout->addWidget(modelList);
    
    mainLayout->addWidget(browseGroup);
    
    QGroupBox* detailsGroup = new QGroupBox(tr("Model Details"), this);
    QVBoxLayout* detailsLayout = new QVBoxLayout(detailsGroup);
    
    modelNameLabel = new QLabel(tr("Model Name: -"), this);
    detailsLayout->addWidget(modelNameLabel);
    
    modelTypeLabel = new QLabel(tr("Model Type: -"), this);
    detailsLayout->addWidget(modelTypeLabel);
    
    modelSizeLabel = new QLabel(tr("Model Size: -"), this);
    detailsLayout->addWidget(modelSizeLabel);
    
    modelPriceLabel = new QLabel(tr("Price: -"), this);
    detailsLayout->addWidget(modelPriceLabel);
    
    modelDescription = new QTextEdit(this);
    modelDescription->setReadOnly(true);
    detailsLayout->addWidget(modelDescription);
    
    mainLayout->addWidget(detailsGroup);
    
    testGroup = new QGroupBox(tr("Model Testing"), this);
    QVBoxLayout* testLayout = new QVBoxLayout(testGroup);
    
    testInput = new QLineEdit(this);
    testInput->setPlaceholderText(tr("Enter test content..."));
    testLayout->addWidget(testInput);
    
    testButton = new QPushButton(tr("Test"), this);
    testLayout->addWidget(testButton);
    
    testOutput = new QTextEdit(this);
    testOutput->setReadOnly(true);
    testLayout->addWidget(testOutput);
    
    mainLayout->addWidget(testGroup);
    
    purchaseGroup = new QGroupBox(tr("Purchase"), this);
    QHBoxLayout* purchaseLayout = new QHBoxLayout(purchaseGroup);
    
    buyModelButton = new QPushButton(tr("Purchase API Key"), this);
    purchaseLayout->addWidget(buyModelButton);
    
    mainLayout->addWidget(purchaseGroup);
    
    connect(browseDirButton, &QPushButton::clicked, this, &ModelPage::BrowseModelDirectory);  // [New]
    connect(modelList, &QListWidget::currentTextChanged, this, &ModelPage::ShowModelDetails);
    connect(testButton, &QPushButton::clicked, this, &ModelPage::TestModel);
    connect(buyModelButton, &QPushButton::clicked, this, &ModelPage::PurchaseModel);
    connect(searchBox, &QLineEdit::textChanged, this, &ModelPage::SearchModels);
    connect(modelTypeFilter, &QComboBox::currentTextChanged, this, &ModelPage::FilterByType);
}

void ModelPage::LoadModelList() {
    // [Modified]Prefer refreshing from loaded directory, otherwise show default hint
    if (!currentModelDir.isEmpty()) {
        LoadModelsFromDir(currentModelDir);
    } else {
        // ShowHintInfo
        modelList->clear();
        modelList->addItem(tr("(Click \"Browse Model Directory\" button to select a folder)"));
        qDebug() << "ModelPage: Waiting for user to select model directory";
    }
}

void ModelPage::ShowModelDetails(const QString& modelName) {
    // [Modified]Get real data from modelDataMap instead of hardcoding
    QString pureName = modelName.split(" (").first();  // Remove type suffix
    
    if (modelDataMap.contains(pureName)) {
        const QMap<QString, QString>& data = modelDataMap[pureName];
        
        modelNameLabel->setText(tr("Model Name: %1").arg(data["name"]));
        modelTypeLabel->setText(tr("Model Type: %1").arg(data["type"]));
        modelSizeLabel->setText(tr("Model Size: %1").arg(data["size"]));
        modelPriceLabel->setText(tr("File Path: %1").arg(data["path"]));
        modelDescription->setPlainText(
            tr("Local AI Model File\n\n"
               "Format: %1\n"
               "Path: %2\n\n"
               "This model can be used for local inference tasks.")
            .arg(data["suffix"].toUpper())
            .arg(data["path"])
        );
    } else {
        // Fallback to default display (when list is empty or not selected)
        modelNameLabel->setText(tr("Model Name: %1").arg(modelName));
        modelTypeLabel->setText(tr("Model Type: -"));
        modelSizeLabel->setText(tr("Model Size: -"));
        modelPriceLabel->setText(tr("Price: -"));
        modelDescription->setPlainText(tr("Please select a model to view details."));
    }
}

void ModelPage::TestModel() {
    QString input = testInput->text();
    if (input.isEmpty()) {
        QMessageBox::warning(this, tr("Warning"), tr("Please enter test content"));
        return;
    }
    
    // [Modified]Check if a model is selected
    if (modelList->currentRow() < 0 || !modelList->currentItem()) {
        QMessageBox::warning(this, tr("Warning"), tr("Please select a model first"));
        return;
    }
    
    QString modelName = modelList->currentItem()->text().split(" (").first();
    
    // [Modified]Show real status info
    testOutput->setPlainText(
        tr("=== Model Test Request ===\n\n"
           "Model: %1\n"
           "Input: %2\n\n"
           "Note: Model inference requires backend service support.\n\n"
           "Current status: Frontend GUI ready\n"
           "Backend status: Waiting for connection\n\n"
           "To enable real inference, ensure:\n"
           "1. Local model file selected (use \"Browse Model Directory\" button)\n"
           "2. Backend inference service started\n"
           "3. RPC interface configured\n\n"
           "This feature will auto-enable after backend integration.")
        .arg(modelName)
        .arg(input)
    );
    
    qDebug() << "ModelPage: TestRequest - Model:" << modelName << "Input:" << input;
}

void ModelPage::PurchaseModel() {
    QMessageBox::information(this, tr("Purchase API Key"), tr("Purchase API Key feature"));
}

// ========== [New]Browse Model Directory ==========
void ModelPage::BrowseModelDirectory() {
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("Select Model Directory"),
        currentModelDir.isEmpty() ? QDir::homePath() : currentModelDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );
    
    if (!dir.isEmpty()) {
        currentModelDir = dir;
        qDebug() << "ModelPage: Selected model directory:" << dir;
        
        // Load model list from directory
        LoadModelsFromDir(dir);
        
        // Save to config file (auto-load on next start)
        QSettings settings("TKNC", "TokenCoin");
        settings.setValue("modelDirectory", dir);
        
        SetStatus(tr("\u2713 Loaded model directory: %1").arg(dir), false);
    }
}

// ========== [New]Load model list from directory ==========
void ModelPage::LoadModelsFromDir(const QString& dirPath) {
    modelList->clear();
    modelDataMap.clear();
    
    QDir dir(dirPath);
    
    // Find common AI model file formats
    QStringList filters;
    filters << "*.bin" << "*.safetensors" << "*.pt" << "*.pth" 
            << "*.onnx" << "*.gguf" << "*.mlmodel"
            << "*.json";  // Configuration file
    
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files | QDir::NoDotAndDotDot);
    
    if (files.isEmpty()) {
        // If no model files found, try finding subdirectories (e.g. HuggingFace format)
        QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        
        for (const QFileInfo& subdir : subdirs) {
            QDir subDir(subdir.absoluteFilePath());
            QFileInfoList subFiles = subDir.entryInfoList(filters, QDir::Files);
            
            for (const QFileInfo& file : subFiles) {
                AddModelToList(file, subdir.baseName());
            }
        }
    } else {
        for (const QFileInfo& file : files) {
            AddModelToList(file, "");
        }
    }
    
    // If still no models found, show hint info
    if (modelList->count() == 0) {
        modelList->addItem(tr("(No model files found)"));
        qWarning() << "ModelPage: No model files found in directory:" << dirPath;
        SetStatus(tr("\u26a0\ufe0f No supported model files found in directory"), true);
    } else {
        qDebug() << "ModelPage: Loaded" << modelList->count() << "Model";
        SetStatus(tr("\u2713 Loaded %1 model(s)").arg(modelList->count()), false);
    }
}

// ========== [New]Helper: add model to list and data map ==========
void ModelPage::AddModelToList(const QFileInfo& fileInfo, const QString& groupName) {
    QString displayName;
    if (groupName.isEmpty()) {
        displayName = fileInfo.completeBaseName();
    } else {
        displayName = groupName + "/" + fileInfo.completeBaseName();
    }
    
    // Determine model type
    QString modelType = DetectModelType(fileInfo.suffix());
    
    // Add to UI list
    QString listItemText = QString("%1 (%2)").arg(displayName, modelType);
    modelList->addItem(listItemText);
    
    // Store detail data to map
    QMap<QString, QString> modelData;
    modelData["name"] = displayName;
    modelData["type"] = modelType;
    modelData["path"] = fileInfo.absoluteFilePath();
    modelData["size"] = FormatFileSize(fileInfo.size());
    modelData["suffix"] = fileInfo.suffix().toLower();
    
    modelDataMap[displayName] = modelData;
}

// ========== [New]Detect model type ==========
QString ModelPage::DetectModelType(const QString& suffix) {
    QString sfx = suffix.toLower();
    
    if (sfx == ".bin" || sfx == ".safetensors") return "text";
    else if (sfx == ".pt" || sfx == ".pth") return "text";
    else if (sfx == ".onnx") return "text";
    else if (sfx == ".gguf") return "text";
    else if (sfx == ".mlmodel") return "coreml";
    else if (sfx == ".json") return "config";
    else return "unknown";
}

// ========== [New]Format file size ==========
QString ModelPage::FormatFileSize(qint64 bytes) {
    const qint64 KB = 1024;
    const qint64 MB = 1024 * KB;
    const qint64 GB = 1024 * MB;
    
    if (bytes >= GB) {
        return QString("%1 GB").arg(double(bytes) / GB, 0, 'f', 2);
    } else if (bytes >= MB) {
        return QString("%1 MB").arg(double(bytes) / MB, 0, 'f', 2);
    } else if (bytes >= KB) {
        return QString("%1 KB").arg(double(bytes) / KB, 0, 'f', 2);
    } else {
        return QString("%1 B").arg(bytes);
    }
}

void ModelPage::SearchModels(const QString& keyword) {
    if (keyword.isEmpty()) {
        LoadModelList();
        return;
    }
    
    // [Modified]Search using modelDataMap
    modelList->clear();
    
    for (auto it = modelDataMap.constBegin(); it != modelDataMap.constEnd(); ++it) {
        const QMap<QString, QString>& data = it.value();
        
        // Search model name or path
        if (data["name"].contains(keyword, Qt::CaseInsensitive) ||
            data["path"].contains(keyword, Qt::CaseInsensitive)) {
            
            QString listItemText = QString("%1 (%2)").arg(data["name"], data["type"]);
            modelList->addItem(listItemText);
        }
    }
    
    if (modelList->count() == 0) {
        modelList->addItem(tr("(No matching models found)"));
    }
    
    qDebug() << "ModelPage: Search finished - keyword:" << keyword << "result count:" << modelList->count();
}

void ModelPage::FilterByType(const QString& type) {
    if (type == "All") {
        LoadModelList();
        return;
    }
    
    // [Modified]Filter using modelDataMap
    modelList->clear();
    
    for (auto it = modelDataMap.constBegin(); it != modelDataMap.constEnd(); ++it) {
        const QMap<QString, QString>& data = it.value();
        
        if (data["type"] == type) {
            QString listItemText = QString("%1 (%2)").arg(data["name"], data["type"]);
            modelList->addItem(listItemText);
        }
    }
    
    if (modelList->count() == 0) {
        modelList->addItem(tr("(No models of type %1 found)").arg(type));
    }
    
    qDebug() << "ModelPage: Filter finished - type:" << type << "result count:" << modelList->count();
}

// ========== [New]Status display helper function ==========
void ModelPage::SetStatus(const QString& message, bool isError) {
    // Use qDebug/qWarning to output status info
    if (isError) {
        qWarning() << "ModelPage[ERROR]:" << message;
    } else {
        qDebug() << "ModelPage:" << message;
    }
    
    // TODO: Can add status bar display in UI (optional)
}
