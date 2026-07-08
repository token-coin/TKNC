#ifndef TKN_QT_MODELPAGE_H
#define TKN_QT_MODELPAGE_H

#include <tknc-build-config.h>
#include <QObject>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QComboBox>
#include <QTextEdit>
#include <QListWidget>
#include <QMap>
#include <QFileInfo>

class ModelPage : public QWidget {
    Q_OBJECT

private:
    QListWidget* modelList;
    QLabel* modelNameLabel;
    QLabel* modelTypeLabel;
    QLabel* modelSizeLabel;
    QLabel* modelPriceLabel;
    QTextEdit* modelDescription;
    
    QLineEdit* testInput;
    QPushButton* testButton;
    QTextEdit* testOutput;
    
    QPushButton* buyModelButton;
    QComboBox* modelTypeFilter;
    QLineEdit* searchBox;
    QPushButton* browseDirButton;
    
    QVBoxLayout* mainLayout;
    QGroupBox* browseGroup;
    QGroupBox* testGroup;
    QGroupBox* purchaseGroup;
    
    QString currentModelDir;
    QMap<QString, QMap<QString, QString>> modelDataMap;

public:
    explicit ModelPage(QWidget* parent = nullptr);

private Q_SLOTS:
    void LoadModelList();
    
    void ShowModelDetails(const QString& modelName);
    
    void TestModel();
    
    void PurchaseModel();
    
    void SearchModels(const QString& keyword);
    
    void FilterByType(const QString& type);
    
    void BrowseModelDirectory();
    
    void LoadModelsFromDir(const QString& dirPath);
    
    void AddModelToList(const QFileInfo& fileInfo, const QString& groupName);
    QString DetectModelType(const QString& suffix);
    QString FormatFileSize(qint64 bytes);
    void SetStatus(const QString& message, bool isError = false);
};

#endif // TKN_QT_MODELPAGE_H
