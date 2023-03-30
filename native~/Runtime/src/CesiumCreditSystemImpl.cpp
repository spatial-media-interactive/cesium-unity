#include "CameraManager.h"
#include "Cesium3DTilesetImpl.h"
#include "UnityTilesetExternals.h"

#include <Cesium3DTilesSelection/IonRasterOverlay.h>
#include <Cesium3DTilesSelection/Tileset.h>

#include <DotNet/CesiumForUnity/Cesium3DTileset.h>
#include <DotNet/CesiumForUnity/CesiumCredit.h>
#include <DotNet/CesiumForUnity/CesiumCreditComponent.h>
#include <DotNet/CesiumForUnity/CesiumCreditSystem.h>
#include <DotNet/CesiumForUnity/CesiumGeoreference.h>
#include <DotNet/CesiumForUnity/CesiumRasterOverlay.h>
#include <DotNet/System/Array1.h>
#include <DotNet/System/Collections/Generic/List1.h>
#include <DotNet/System/Collections/IEnumerator.h>
#include <DotNet/System/Object.h>
#include <DotNet/System/String.h>
#include <DotNet/UnityEngine/Coroutine.h>
#include <DotNet/UnityEngine/Debug.h>
#include <DotNet/UnityEngine/GameObject.h>
#include <DotNet/UnityEngine/HideFlags.h>
#include <DotNet/UnityEngine/Object.h>
#include <DotNet/UnityEngine/Resources.h>
#include <DotNet/UnityEngine/Texture2D.h>
#include <DotNet/UnityEngine/Transform.h>
#include <tidybuffio.h>

#if UNITY_EDITOR
#include <DotNet/UnityEditor/EditorApplication.h>
#endif

using namespace Cesium3DTilesSelection;
using namespace DotNet;
using namespace System::Collections::Generic;

namespace CesiumForUnityNative {

CesiumCreditSystemImpl::CesiumCreditSystemImpl(
    const CesiumForUnity::CesiumCreditSystem& creditSystem)
    : _pCreditSystem(std::make_shared<CreditSystem>()),
      _htmlToUnityCredit(),
      _lastCreditsCount(0),
      _creditsUpdated(false) {}

CesiumCreditSystemImpl::~CesiumCreditSystemImpl() {}

void CesiumCreditSystemImpl::Update(
    const CesiumForUnity::CesiumCreditSystem& creditSystem) {
  if (!_pCreditSystem) {
    return;
  }

  // If the credits were updated in a previous frame, the update will not get
  // broadcasted until all of the images are fully loaded. Broadcast the
  // previous update first before handling the next one.
  if (_creditsUpdated && !creditSystem.HasLoadingImages()) {
    creditSystem.BroadcastCreditsUpdate();
    _creditsUpdated = false;
  }

  const std::vector<Cesium3DTilesSelection::Credit>& creditsToShowThisFrame =
      _pCreditSystem->getCreditsToShowThisFrame();

  // If the credit list has changed, reformat the credits.
  _creditsUpdated =
      creditsToShowThisFrame.size() != _lastCreditsCount ||
      _pCreditSystem->getCreditsToNoLongerShowThisFrame().size() > 0;

  if (_creditsUpdated) {
    List1<CesiumForUnity::CesiumCredit> popupCredits =
        creditSystem.popupCredits();
    List1<CesiumForUnity::CesiumCredit> onScreenCredits =
        creditSystem.onScreenCredits();

    popupCredits.Clear();
    onScreenCredits.Clear();

    size_t creditsCount = creditsToShowThisFrame.size();

    for (int i = 0; i < creditsCount; i++) {
      const Cesium3DTilesSelection::Credit& credit = creditsToShowThisFrame[i];

      DotNet::CesiumForUnity::CesiumCredit unityCredit;
      const std::string& html = _pCreditSystem->getHtml(credit);

      auto htmlFind = _htmlToUnityCredit.find(html);
      if (htmlFind != _htmlToUnityCredit.end()) {
        unityCredit = htmlFind->second;
      } else {
        unityCredit = convertHtmlToUnityCredit(html, creditSystem);
        _htmlToUnityCredit.insert({html, unityCredit});
      }

      if (_pCreditSystem->shouldBeShownOnScreen(credit)) {
        onScreenCredits.Add(unityCredit);
      } else {
        popupCredits.Add(unityCredit);
      }
    }

    _lastCreditsCount = creditsCount;
  }

  _pCreditSystem->startNextFrame();
}

namespace {
void htmlToCreditComponents(
    TidyDoc tdoc,
    TidyNode tnod,
    std::string& parentUrl,
    List1<CesiumForUnity::CesiumCreditComponent>& componentList,
    const CesiumForUnity::CesiumCreditSystem& creditSystem) {
  TidyNode child;
  TidyBuffer buf;
  tidyBufInit(&buf);

  for (child = tidyGetChild(tnod); child; child = tidyGetNext(child)) {
    System::String componentText("");
    System::String componentLink("");
    int componentImageId = -1;

    if (tidyNodeIsText(child)) {
      tidyNodeGetText(tdoc, child, &buf);

      if (buf.bp) {
        std::string text = reinterpret_cast<const char*>(buf.bp);
        tidyBufClear(&buf);

        // could not find correct option in tidy html to not add new lines
        if (text.size() != 0 && text[text.size() - 1] == '\n') {
          text.pop_back();
        }

        componentText = System::String(text);
      }
    } else if (tidyNodeGetId(child) == TidyTagId::TidyTag_IMG) {
      auto srcAttr = tidyAttrGetById(child, TidyAttrId::TidyAttr_SRC);

      if (srcAttr) {
        auto srcValue = tidyAttrValue(srcAttr);
        if (srcValue) {
          // Get the number of images that existed before LoadImage is called.
          componentImageId = creditSystem.images().Count();

          const std::string srcString =
              std::string(reinterpret_cast<const char*>(srcValue));
          creditSystem.StartCoroutine(
              creditSystem.LoadImage(System::String(srcString)));
        }
      }
    }

    if (!System::String::IsNullOrEmpty(componentText) ||
        componentImageId >= 0) {
      if (!parentUrl.empty()) {
        componentLink = System::String(parentUrl);
      }

      CesiumForUnity::CesiumCreditComponent component(
          componentText,
          componentLink,
          componentImageId);
      componentList.Add(component);
    }

    auto hrefAttr = tidyAttrGetById(child, TidyAttrId::TidyAttr_HREF);
    if (hrefAttr) {
      auto hrefValue = tidyAttrValue(hrefAttr);
      parentUrl = std::string(reinterpret_cast<const char*>(hrefValue));
    }

    htmlToCreditComponents(tdoc, child, parentUrl, componentList, creditSystem);
  }

  tidyBufFree(&buf);
}
} // namespace

const CesiumForUnity::CesiumCredit
CesiumCreditSystemImpl::convertHtmlToUnityCredit(
    const std::string& html,
    const CesiumForUnity::CesiumCreditSystem& creditSystem) {
  CesiumForUnity::CesiumCredit credit = CesiumForUnity::CesiumCredit();

  TidyDoc tdoc;
  TidyBuffer tidy_errbuf = {0};
  int err;

  tdoc = tidyCreate();
  tidyOptSetBool(tdoc, TidyForceOutput, yes);
  tidyOptSetInt(tdoc, TidyWrapLen, 0);
  tidyOptSetInt(tdoc, TidyNewline, TidyLF);

  tidySetErrorBuffer(tdoc, &tidy_errbuf);

  std::string modifiedHtml =
      "<!DOCTYPE html><html><body>" + html + "</body></html>";

  std::string url;
  err = tidyParseString(tdoc, modifiedHtml.c_str());
  if (err < 2) {
    htmlToCreditComponents(
        tdoc,
        tidyGetRoot(tdoc),
        url,
        credit.components(),
        creditSystem);
  }
  tidyBufFree(&tidy_errbuf);
  tidyRelease(tdoc);

  return credit;
}

const std::shared_ptr<Cesium3DTilesSelection::CreditSystem>&
CesiumCreditSystemImpl::getExternalCreditSystem() const {
  return _pCreditSystem;
}

} // namespace CesiumForUnityNative
