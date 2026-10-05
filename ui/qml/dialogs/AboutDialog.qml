import QtQuick
import SUBstation

// Help › About SUBstation (QMessageBox.about): the application's icon, its
// name, version and what it is (Session.aboutTitle, Session.aboutText).
MessageBox {
    objectName: "aboutDialog"
    title: Session.aboutTitle
    text: Session.aboutText
    icon: "about"
}
