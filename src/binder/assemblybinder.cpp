// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
// ============================================================
//
// AssemblyBinder.cpp
//


//
// Implements the AssemblyBinder class
//
// ============================================================

#include "assemblybinder.hpp"
#include "assemblyname.hpp"

#include "assembly.hpp"
#include "applicationcontext.hpp"
#include "loadcontext.hpp"
#include "bindresult.inl"
#include "failurecache.hpp"
#ifdef FEATURE_VERSIONING_LOG
#include "bindinglog.hpp"
#endif // FEATURE_VERSIONING_LOG
#include "utils.hpp"
#include "variables.hpp"
#include "stringarraylist.h"

#include "strongname.h"
#ifdef FEATURE_VERSIONING_LOG
#include "../dlls/mscorrc/fusres.h"
#endif // FEATURE_VERSIONING_LOG

#define APP_DOMAIN_LOCKED_UNLOCKED        0x02
#define APP_DOMAIN_LOCKED_CONTEXT         0x04

#ifndef IMAGE_FILE_MACHINE_ARM64
#define IMAGE_FILE_MACHINE_ARM64             0xAA64  // ARM64 Little-Endian
#endif

BOOL IsCompilationProcess();

#if !defined(DACCESS_COMPILE) && !defined(CROSSGEN_COMPILE)
#include "clrprivbindercoreclr.h"
#include "clrprivbinderassemblyloadcontext.h"
// Helper function in the VM, invoked by the Binder, to invoke the host assembly resolver
extern HRESULT RuntimeInvokeHostAssemblyResolver(INT_PTR pManagedAssemblyLoadContextToBindWithin, 
                                                IAssemblyName *pIAssemblyName, CLRPrivBinderCoreCLR *pTPABinder,
                                                BINDER_SPACE::AssemblyName *pAssemblyName, ICLRPrivAssembly **ppLoadedAssembly);

#endif // !defined(DACCESS_COMPILE) && !defined(CROSSGEN_COMPILE)

namespace BINDER_SPACE
{
    namespace
    {
        //
        // This defines the assembly equivalence relation
        //
        HRESULT IsValidAssemblyVersion(/* in */ AssemblyName *pRequestedName,
                                       /* in */ AssemblyName *pFoundName,
                                       /* in */ ApplicationContext *pApplicationContext)
        {
            HRESULT hr = S_OK;
            BINDER_LOG_ENTER(W("IsValidAssemblyVersion"));
            AssemblyVersion *pRequestedVersion = pRequestedName->GetVersion();
            AssemblyVersion *pFoundVersion = pFoundName->GetVersion();

            do
            {
                if (!pRequestedVersion->HasMajor())
                {
                    // An unspecified requested version component matches any value for the same component in the found version,
                    // regardless of lesser-order version components
                    break;
                }
                if (!pFoundVersion->HasMajor() || pRequestedVersion->GetMajor() > pFoundVersion->GetMajor())
                {
                    // - A specific requested version component does not match an unspecified value for the same component in
                    //   the found version, regardless of lesser-order version components
                    // - Or, the requested version is greater than the found version
                    hr = FUSION_E_APP_DOMAIN_LOCKED;
                    break;
                }
                if (pRequestedVersion->GetMajor() < pFoundVersion->GetMajor())
                {
                    // The requested version is less than the found version
                    break;
                }

                if (!pRequestedVersion->HasMinor())
                {
                    break;
                }
                if (!pFoundVersion->HasMinor() || pRequestedVersion->GetMinor() > pFoundVersion->GetMinor())
                {
                    hr = FUSION_E_APP_DOMAIN_LOCKED;
                    break;
                }
                if (pRequestedVersion->GetMinor() < pFoundVersion->GetMinor())
                {
                    break;
                }

                if (!pRequestedVersion->HasBuild())
                {
                    break;
                }
                if (!pFoundVersion->HasBuild() || pRequestedVersion->GetBuild() > pFoundVersion->GetBuild())
                {
                    hr = FUSION_E_APP_DOMAIN_LOCKED;
                    break;
                }
                if (pRequestedVersion->GetBuild() < pFoundVersion->GetBuild())
                {
                    break;
                }

                if (!pRequestedVersion->HasRevision())
                {
                    break;
                }
                if (!pFoundVersion->HasRevision() || pRequestedVersion->GetRevision() > pFoundVersion->GetRevision())
                {
                    hr = FUSION_E_APP_DOMAIN_LOCKED;
                    break;
                }
            } while (false);

            if (pApplicationContext->IsTpaListProvided() && hr == FUSION_E_APP_DOMAIN_LOCKED)
            {
                // For our new binding models, use a more descriptive error code than APP_DOMAIN_LOCKED for bind
                // failures.
                hr = FUSION_E_REF_DEF_MISMATCH;
            }

            BINDER_LOG_LEAVE_HR(W("IsValidAssemblyVersion"), hr)
            return hr;
        }

        HRESULT URLToFullPath(PathString &assemblyPath)
        {
            HRESULT hr = S_OK;

            SString::Iterator pos = assemblyPath.Begin();
            if (assemblyPath.MatchCaseInsensitive(pos, g_BinderVariables->httpURLPrefix))
            {
                // HTTP downloads are unsupported
                hr = FUSION_E_CODE_DOWNLOAD_DISABLED;
            }
            else
            {
                SString fullAssemblyPath;
                WCHAR *pwzFullAssemblyPath = fullAssemblyPath.OpenUnicodeBuffer(MAX_LONGPATH);
                DWORD dwCCFullAssemblyPath = MAX_LONGPATH + 1; // SString allocates extra byte for null.

                MutateUrlToPath(assemblyPath);

                dwCCFullAssemblyPath = WszGetFullPathName(assemblyPath.GetUnicode(),
                                                          dwCCFullAssemblyPath,
                                                          pwzFullAssemblyPath,
                                                          NULL);
                if (dwCCFullAssemblyPath > MAX_LONGPATH)
                {
                    fullAssemblyPath.CloseBuffer();
                    pwzFullAssemblyPath = fullAssemblyPath.OpenUnicodeBuffer(dwCCFullAssemblyPath - 1);
                    dwCCFullAssemblyPath = WszGetFullPathName(assemblyPath.GetUnicode(),
                                                              dwCCFullAssemblyPath,
                                                              pwzFullAssemblyPath,
                                                              NULL);
                }
                fullAssemblyPath.CloseBuffer(dwCCFullAssemblyPath);

                if (dwCCFullAssemblyPath == 0)
                {
                    hr = HRESULT_FROM_GetLastError();
                }
                else
                {
                    assemblyPath.Set(fullAssemblyPath);
                }
            }

            return hr;
        }


#ifdef FEATURE_VERSIONING_LOG
        //
        // This function outputs the current binding result
        // and flushes the bind log.
        //
        HRESULT LogBindResult(ApplicationContext *pApplicationContext,
                              HRESULT             hrLog,
                              BindResult         *pBindResult)
        {
            HRESULT hr = S_OK;
            BindingLog *pBindingLog = pApplicationContext->GetBindingLog();

            if (!pBindingLog->CanLog())
            {
                // For non-logging, return the bind result
                hr = hrLog;
                goto Exit;
            }

            IF_FAIL_GO(pBindingLog->LogHR(hrLog));

            if ((hrLog == S_OK) && pBindResult->HaveResult())
            {
                IF_FAIL_GO(pBindingLog->LogResult(pBindResult));
            }

            IF_FAIL_GO(pBindingLog->Flush());

            // For failure-free logging, return the bind result
            hr = hrLog;

        Exit:
            // SilverLight does not propagate binding log; therefore kill the information here.
            pApplicationContext->ClearBindingLog();
            return hr;
        }

        HRESULT LogAppDomainLocked(ApplicationContext *pApplicationContext,
                                   DWORD               dwLockedReason,
                                   AssemblyName       *pAssemblyName = NULL)
        {
            HRESULT hr = S_OK;
            BindingLog *pBindingLog = pApplicationContext->GetBindingLog();

            if (pBindingLog->CanLog())
            {
                PathString info;
                PathString format;

                switch (dwLockedReason)
                {
                case APP_DOMAIN_LOCKED_UNLOCKED:
                {
                    IF_FAIL_GO(info.
                               LoadResourceAndReturnHR(CCompRC::Debugging,
                                                       ID_FUSLOG_BINDING_LOCKED_UNLOCKED));
                }
                break;
                case APP_DOMAIN_LOCKED_CONTEXT:
                {
                    PathString displayName;

                    _ASSERTE(pAssemblyName != NULL);

                    IF_FAIL_GO(format.
                               LoadResourceAndReturnHR(CCompRC::Debugging,
                                                       ID_FUSLOG_BINDING_LOCKED_ASSEMBLY_EXE_CONTEXT));

                    pAssemblyName->GetDisplayName(displayName,
                                                  AssemblyName::INCLUDE_VERSION |
                                                  AssemblyName::INCLUDE_ARCHITECTURE);

                    info.Printf(format.GetUnicode(), displayName.GetUnicode());
                }
                break;
                default:
                    _ASSERTE(0);
                    IF_FAIL_GO(E_INVALIDARG);
                    break;
                }

                IF_FAIL_GO(pBindingLog->Log(info));
            }

        Exit:
            return hr;
        }

        HRESULT LogAssemblyNameWhereRef(ApplicationContext *pApplicationContext,
                                        Assembly           *pAssembly)
        {
            HRESULT hr = S_OK;
            BindingLog *pBindingLog = pApplicationContext->GetBindingLog();

            if (pBindingLog->CanLog())
            {
                PathString info;

                IF_FAIL_GO(info.
                           LoadResourceAndReturnHR(CCompRC::Debugging, ID_FUSLOG_BINDING_LOG_WHERE_REF_NAME));
                IF_FAIL_GO(pBindingLog->LogAssemblyName(info.GetUnicode(),
                                                        pAssembly->GetAssemblyName()));
            }
            
        Exit:
            return hr;
        }

        HRESULT LogConfigurationError(ApplicationContext *pApplicationContext,
                                      AssemblyName       *pCulturedManifestName,
                                      AssemblyName       *pLocalPathAssemblyName)
        {
            HRESULT hr = S_OK;
            BindingLog *pBindingLog = pApplicationContext->GetBindingLog();

            if (pBindingLog->CanLog())
            {
                PathString tmp;
                PathString culturedManifestDisplayName;
                PathString localPathDisplayName;
                PathString info;

                IF_FAIL_GO(tmp.
                           LoadResourceAndReturnHR(CCompRC::Debugging, ID_FUSLOG_BINDING_LOG_ERRONOUS_MANIFEST_ENTRY));

                pCulturedManifestName->GetDisplayName(culturedManifestDisplayName,
                                                      AssemblyName::INCLUDE_VERSION |
                                                      AssemblyName::INCLUDE_ARCHITECTURE);
                pLocalPathAssemblyName->GetDisplayName(localPathDisplayName,
                                                       AssemblyName::INCLUDE_VERSION |
                                                       AssemblyName::INCLUDE_ARCHITECTURE);
                
                info.Printf(tmp.GetUnicode(),
                            culturedManifestDisplayName.GetUnicode(), 
                            localPathDisplayName.GetUnicode());
                IF_FAIL_GO(pBindingLog->Log(info.GetUnicode()));
            }

        Exit:
            return hr;
        }
#endif // FEATURE_VERSIONING_LOG

#ifndef CROSSGEN_COMPILE
        HRESULT CreateImageAssembly(IMDInternalImport       *pIMetaDataAssemblyImport,
                                    PEKIND                   PeKind,
                                    PEImage                 *pPEImage,
                                    PEImage                 *pNativePEImage,
                                    BindResult              *pBindResult)
        {
            HRESULT hr = S_OK;
            ReleaseHolder<Assembly> pAssembly;
            PathString asesmblyPath;

            SAFE_NEW(pAssembly, Assembly);
            IF_FAIL_GO(pAssembly->Init(pIMetaDataAssemblyImport,
                                       PeKind,
                                       pPEImage,
                                       pNativePEImage,
                                       asesmblyPath,
                                       FALSE /* fIsInGAC */));
            
            pBindResult->SetResult(pAssembly);
            pBindResult->SetIsFirstRequest(TRUE);

        Exit:
            return hr;
        }
#endif // !CROSSGEN_COMPILE
    };

    /* static */
    HRESULT AssemblyBinder::Startup()
    {
        STATIC_CONTRACT_NOTHROW;

        HRESULT hr = S_OK;
 
        // This should only be called once
        _ASSERTE(g_BinderVariables == NULL);
        g_BinderVariables = new Variables();
        IF_FAIL_GO(g_BinderVariables->Init());

        // Setup Debug log
        BINDER_LOG_STARTUP();

    Exit:
        return hr;
    }


    HRESULT AssemblyBinder::TranslatePEToArchitectureType(DWORD  *pdwPAFlags, PEKIND *PeKind)
    {
        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("TranslatePEToArchitectureType"))

        _ASSERTE(pdwPAFlags != NULL);
        _ASSERTE(PeKind != NULL);

        CorPEKind CLRPeKind = (CorPEKind) pdwPAFlags[0];
        DWORD dwImageType = pdwPAFlags[1];

        *PeKind = peNone;

        if(CLRPeKind == peNot) 
        {
            // Not a PE. Shouldn't ever get here.
            BINDER_LOG(W("Not a PE!"));
            IF_FAIL_GO(HRESULT_FROM_WIN32(ERROR_BAD_FORMAT));
        }
        else 
        {
            if ((CLRPeKind & peILonly) && !(CLRPeKind & pe32Plus) &&
                !(CLRPeKind & pe32BitRequired) && dwImageType == IMAGE_FILE_MACHINE_I386) 
            {
                // Processor-agnostic (MSIL)
                BINDER_LOG(W("Processor-agnostic (MSIL)"));
                *PeKind = peMSIL;
            }
            else if (CLRPeKind & pe32Plus) 
            {
                // 64-bit
                if (CLRPeKind & pe32BitRequired) 
                {
                    // Invalid
                    BINDER_LOG(W("CLRPeKind & pe32BitRequired is true"));
                    IF_FAIL_GO(HRESULT_FROM_WIN32(ERROR_BAD_FORMAT));
                }

                // Regardless of whether ILONLY is set or not, the architecture
                // is the machine type.
                if(dwImageType == IMAGE_FILE_MACHINE_ARM64) 
                    *PeKind = peARM64;
                else if (dwImageType == IMAGE_FILE_MACHINE_AMD64) 
                    *PeKind = peAMD64;
                else 
                {
                    // We don't support other architectures
                    BINDER_LOG(W("Unknown architecture"));
                    IF_FAIL_GO(HRESULT_FROM_WIN32(ERROR_BAD_FORMAT));
                }
            }
            else
            {
                // 32-bit, non-agnostic
                if(dwImageType == IMAGE_FILE_MACHINE_I386) 
                    *PeKind = peI386;
                else if(dwImageType == IMAGE_FILE_MACHINE_ARMNT) 
                    *PeKind = peARM;
                else 
                {
                    // Not supported
                    BINDER_LOG(W("32-bit, non-agnostic"));
                    IF_FAIL_GO(HRESULT_FROM_WIN32(ERROR_BAD_FORMAT));
                }
            }
        }

    Exit:
        BINDER_LOG_LEAVE_HR(W("TranslatePEToArchitectureType"), hr);
        return hr;
    }
        
    // See code:BINDER_SPACE::AssemblyBinder::GetAssembly for info on fNgenExplicitBind
    // and fExplicitBindToNativeImage, and see code:CEECompileInfo::LoadAssemblyByPath
    // for an example of how they're used.
    HRESULT AssemblyBinder::BindAssembly(/* in */  ApplicationContext  *pApplicationContext,
                                         /* in */  AssemblyName        *pAssemblyName,
                                         /* in */  LPCWSTR              szCodeBase,
                                         /* in */  PEAssembly          *pParentAssembly,
                                         /* in */  BOOL                 fNgenExplicitBind,
                                         /* in */  BOOL                 fExplicitBindToNativeImage,
                                         /* in */  bool                 excludeAppPaths,
                                         /* out */ Assembly           **ppAssembly)
    {
        HRESULT hr = S_OK;
        LONG kContextVersion = 0;
        BindResult bindResult;
        
        BINDER_LOG_ENTER(W("AssemblyBinder::BindAssembly"));

#ifndef CROSSGEN_COMPILE
    Retry:
        {
            // Lock the binding application context
            CRITSEC_Holder contextLock(pApplicationContext->GetCriticalSectionCookie());
#endif

            if (szCodeBase == NULL)
            {
#ifdef FEATURE_VERSIONING_LOG
                // Log bind
                IF_FAIL_GO(BindingLog::CreateInContext(pApplicationContext,
                                                       pAssemblyName,
                                                       pParentAssembly));
#endif // FEATURE_VERSIONING_LOG


                IF_FAIL_GO(BindByName(pApplicationContext,
                                      pAssemblyName,
                                      false, // skipFailureCaching
                                      false, // skipVersionCompatibilityCheck
                                      excludeAppPaths,
                                      &bindResult));
            }
            else
            {
                PathString assemblyPath(szCodeBase);

#ifdef FEATURE_VERSIONING_LOG
                // Log bind
                IF_FAIL_GO(BindingLog::CreateInContext(pApplicationContext,
                                                       assemblyPath,
                                                       pParentAssembly));
#endif // FEATURE_VERSIONING_LOG

                // Convert URL to full path and block HTTP downloads
                IF_FAIL_GO(URLToFullPath(assemblyPath));
                BOOL fDoNgenExplicitBind = fNgenExplicitBind;
                
                // Only use explicit ngen binding in the new coreclr path-based binding model
                if (!pApplicationContext->IsTpaListProvided())
                {
                    fDoNgenExplicitBind = FALSE;
                }
                
                IF_FAIL_GO(BindWhereRef(pApplicationContext,
                                        assemblyPath,
                                        fDoNgenExplicitBind,
                                        fExplicitBindToNativeImage,
                                        excludeAppPaths,
                                        &bindResult));
            }

            // Remember the post-bind version
            kContextVersion = pApplicationContext->GetVersion();

        Exit:
#ifdef FEATURE_VERSIONING_LOG
            hr = LogBindResult(pApplicationContext, hr, &bindResult);
#else // FEATURE_VERSIONING_LOG
            ;
#endif // FEATURE_VERSIONING_LOG

#ifndef CROSSGEN_COMPILE
        } // lock(pApplicationContext)
#endif

        if (bindResult.HaveResult())
        {

#ifndef CROSSGEN_COMPILE
            BindResult hostBindResult;

            hr = RegisterAndGetHostChosen(pApplicationContext,
                                          kContextVersion,
                                          &bindResult,
                                          &hostBindResult);
            
            if (hr == S_FALSE)
            {
                // Another bind interfered. We need to retry the entire bind.
                // This by design loops as long as needed because by construction we eventually
                // will succeed or fail the bind.
                bindResult.Reset();
                goto Retry;
            }
            else if (hr == S_OK)
            {
                *ppAssembly = hostBindResult.GetAsAssembly(TRUE /* fAddRef */);
            }
#else // CROSSGEN_COMPILE

            *ppAssembly = bindResult.GetAsAssembly(TRUE /* fAddRef */);

#endif // CROSSGEN_COMPILE
        }

        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::BindAssembly"), hr);
        return hr;
    }

    /* static */
    HRESULT AssemblyBinder::BindToSystem(SString   &systemDirectory,
                                         Assembly **ppSystemAssembly,
                                         bool       fBindToNativeImage)
    {
        // Indirect check that binder was initialized.
        _ASSERTE(g_BinderVariables != NULL);

        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("AssemblyBinder:BindToSystem"));

        _ASSERTE(ppSystemAssembly != NULL);

        StackSString sCoreLibDir(systemDirectory);
        ReleaseHolder<Assembly> pSystemAssembly;

        if (!sCoreLibDir.EndsWith(DIRECTORY_SEPARATOR_CHAR_W))
        {
            sCoreLibDir.Append(DIRECTORY_SEPARATOR_CHAR_W);
        }

        StackSString sCoreLib;

        // At run-time, System.Private.CoreLib.dll is expected to be the NI image.
        sCoreLib = sCoreLibDir;
        sCoreLib.Append(CoreLibName_IL_W);
        IF_FAIL_GO(AssemblyBinder::GetAssembly(sCoreLib,
                                               TRUE /* fIsInGAC */,
                                               fBindToNativeImage,
                                               &pSystemAssembly));
        
        *ppSystemAssembly = pSystemAssembly.Extract();

    Exit:
        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::BindToSystem"), hr);
        return hr;
    }


    /* static */
    HRESULT AssemblyBinder::BindToSystemSatellite(SString   &systemDirectory,
                                                  SString   &simpleName,
                                                  SString   &cultureName,
                                                  Assembly **ppSystemAssembly)
    {
        // Indirect check that binder was initialized.
        _ASSERTE(g_BinderVariables != NULL);

        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("AssemblyBinder:BindToSystemSatellite"));

        _ASSERTE(ppSystemAssembly != NULL);

        StackSString sMscorlibSatellite(systemDirectory);
        ReleaseHolder<Assembly> pSystemAssembly;

        // append culture name
        if (!cultureName.IsEmpty())
        {
            CombinePath(sMscorlibSatellite, cultureName, sMscorlibSatellite);
        }

        // append satellite assembly's simple name
        CombinePath(sMscorlibSatellite, simpleName, sMscorlibSatellite);

        // append extension
        sMscorlibSatellite.Append(W(".dll"));

        IF_FAIL_GO(AssemblyBinder::GetAssembly(sMscorlibSatellite,
                                               TRUE /* fIsInGAC */,
                                               FALSE /* fExplicitBindToNativeImage */,
                                               &pSystemAssembly));

        *ppSystemAssembly = pSystemAssembly.Extract();

    Exit:
        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::BindToSystemSatellite"), hr);
        return hr;
    }

    /* static */
    HRESULT AssemblyBinder::BindByName(ApplicationContext *pApplicationContext,
                                       AssemblyName       *pAssemblyName,
                                       bool                skipFailureCaching,
                                       bool                skipVersionCompatibilityCheck,
                                       bool                excludeAppPaths,
                                       BindResult         *pBindResult)
    {
        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("AssemblyBinder::BindByName"));
        PathString assemblyDisplayName;

        // Look for already cached binding failure (ignore PA, every PA will lock the context)
        pAssemblyName->GetDisplayName(assemblyDisplayName,
                                      AssemblyName::INCLUDE_VERSION);

        hr = pApplicationContext->GetFailureCache()->Lookup(assemblyDisplayName);
        if (FAILED(hr))
        {
            if ((hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) && skipFailureCaching)
            {
                // Ignore pre-existing transient bind error (re-bind will succeed)
                pApplicationContext->GetFailureCache()->Remove(assemblyDisplayName);
            }

            goto LogExit;
        }
        else if (hr == S_FALSE)
        {
            // workaround: Special case for byte arrays. Rerun the bind to create binding log.
            pAssemblyName->SetIsDefinition(TRUE);
            hr = S_OK;
        }

        if (!Assembly::IsValidArchitecture(pAssemblyName->GetArchitecture()))
        {
            // Assembly reference contains wrong architecture
            IF_FAIL_GO(FUSION_E_INVALID_NAME);
        }

        IF_FAIL_GO(BindLocked(pApplicationContext,
                              pAssemblyName,
                              skipVersionCompatibilityCheck,
                              excludeAppPaths,
                              pBindResult));

        if (!pBindResult->HaveResult())
        {
            // Behavior rules are clueless now
            IF_FAIL_GO(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        }

    Exit:
        if (FAILED(hr))
        {
            if (skipFailureCaching)
            {
                if (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                {
                    // Cache non-transient bind error for byte-array
                    hr = S_FALSE;
                }
                else
                {
                    // Ignore transient bind error (re-bind will succeed)
                    goto LogExit;
                }
            }

            hr = pApplicationContext->AddToFailureCache(assemblyDisplayName, hr);
        }
    LogExit:

        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::BindByName"), hr);
        return hr;
    }

    /* static */
    // See code:BINDER_SPACE::AssemblyBinder::GetAssembly for info on fNgenExplicitBind
    // and fExplicitBindToNativeImage, and see code:CEECompileInfo::LoadAssemblyByPath
    // for an example of how they're used.
    HRESULT AssemblyBinder::BindWhereRef(ApplicationContext *pApplicationContext,
                                         PathString         &assemblyPath,
                                         BOOL                fNgenExplicitBind,
                                         BOOL                fExplicitBindToNativeImage,
                                         bool                excludeAppPaths,
                                         BindResult         *pBindResult)
    {
        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("AssemblyBinder::BindWhereRef"));

        ReleaseHolder<Assembly> pAssembly;
        BindResult lockedBindResult;

        // Look for already cached binding failure
        hr = pApplicationContext->GetFailureCache()->Lookup(assemblyPath);
        if (FAILED(hr))
        {
            goto LogExit;
        }

        // If we return this assembly, then it is guaranteed to be not in GAC
        // Design decision. For now, keep the V2 model of Fusion being oblivious of the strong name.
        // Security team did not see any security concern with interpreting the version information.
        IF_FAIL_GO(GetAssembly(assemblyPath,
                               FALSE /* fIsInGAC */,
                               
                               // Pass through caller's intent of whether to bind to the
                               // NI using an explicit path to the NI that was
                               // specified.  Generally only NGEN PDB generation has
                               // this TRUE.
                               fExplicitBindToNativeImage,
                               &pAssembly));
#ifdef FEATURE_VERSIONING_LOG
        IF_FAIL_GO(LogAssemblyNameWhereRef(pApplicationContext, pAssembly));
#endif // FEATURE_VERSIONING_LOG

        AssemblyName *pAssemblyName;
        pAssemblyName = pAssembly->GetAssemblyName();

        if (!fNgenExplicitBind)
        {
            IF_FAIL_GO(BindLocked(pApplicationContext,
                                  pAssemblyName,
                                  false, // skipVersionCompatibilityCheck
                                  excludeAppPaths,
                                  &lockedBindResult));
            if (lockedBindResult.HaveResult())
            {
                pBindResult->SetResult(&lockedBindResult);
                GO_WITH_HRESULT(S_OK);
            }
        }

        hr = S_OK;
        pBindResult->SetResult(pAssembly);

    Exit:

        if (FAILED(hr))
        {
            // Always cache binding failures
            hr = pApplicationContext->AddToFailureCache(assemblyPath, hr);
        }

    LogExit:

        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::BindWhereRef"), hr);
        return hr;
    }

    /* static */
    HRESULT AssemblyBinder::BindLocked(ApplicationContext *pApplicationContext,
                                       AssemblyName       *pAssemblyName,
                                       bool                skipVersionCompatibilityCheck,
                                       bool                excludeAppPaths,
                                       BindResult         *pBindResult)
    {
        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("AssemblyBinder::BindLocked"));
        
#ifndef CROSSGEN_COMPILE
        ContextEntry *pContextEntry = NULL;
        IF_FAIL_GO(FindInExecutionContext(pApplicationContext, pAssemblyName, &pContextEntry));
        if (pContextEntry != NULL)
        {
#if !defined(DACCESS_COMPILE) && !defined(CROSSGEN_COMPILE)
            if (skipVersionCompatibilityCheck)
            {
                // Skip RefDef matching if we have been asked to.
            }
#endif // !defined(DACCESS_COMPILE) && !defined(CROSSGEN_COMPILE)
            else
            {
                // Can't give higher version than already bound
                IF_FAIL_GO(IsValidAssemblyVersion(pAssemblyName, pContextEntry->GetAssemblyName(), pApplicationContext));
            }
            
            pBindResult->SetResult(pContextEntry);
        }
        else
#endif // !CROSSGEN_COMPILE
        if (pApplicationContext->IsTpaListProvided())
        {
            IF_FAIL_GO(BindByTpaList(pApplicationContext,
                                     pAssemblyName,
                                     excludeAppPaths,
                                     pBindResult));
            if (pBindResult->HaveResult())
            {
                hr = IsValidAssemblyVersion(pAssemblyName, pBindResult->GetAssemblyName(), pApplicationContext);
                if (FAILED(hr))
                {
                    pBindResult->SetNoResult();
                }
            }
        }
    Exit:
        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::BindLocked"), hr);
        return hr;
    }

#ifndef CROSSGEN_COMPILE
    /* static */
    HRESULT AssemblyBinder::FindInExecutionContext(ApplicationContext  *pApplicationContext,
                                                   AssemblyName        *pAssemblyName,
                                                   ContextEntry       **ppContextEntry)
    {
        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("AssemblyBinder::FindInExecutionContext"));

        _ASSERTE(pApplicationContext != NULL);
        _ASSERTE(pAssemblyName != NULL);
        _ASSERTE(ppContextEntry != NULL);

        ExecutionContext *pExecutionContext = pApplicationContext->GetExecutionContext();
        ContextEntry *pContextEntry = pExecutionContext->Lookup(pAssemblyName);

        if (pContextEntry != NULL)
        {
            AssemblyName *pContextName = pContextEntry->GetAssemblyName();

#ifdef FEATURE_VERSIONING_LOG
            // First-time requests are considered unlocked, everything else is locked
            DWORD dwLockedReason = (pContextEntry->GetIsFirstRequest() ?
                                    APP_DOMAIN_LOCKED_UNLOCKED : APP_DOMAIN_LOCKED_CONTEXT);

            IF_FAIL_GO(LogAppDomainLocked(pApplicationContext, dwLockedReason, pContextName));
            pContextEntry->SetIsFirstRequest(FALSE);
#endif // FEATURE_VERSIONING_LOG

            if (pAssemblyName->GetIsDefinition() &&
                (pContextName->GetArchitecture() != pAssemblyName->GetArchitecture()))
            {
                IF_FAIL_GO(FUSION_E_APP_DOMAIN_LOCKED);
            }
        }

        *ppContextEntry = pContextEntry;

    Exit:
        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::FindInExecutionContext"), hr);
        return hr;
    }

#endif //CROSSGEN_COMPILE

    //
    // Tests whether a candidate assembly's name matches the requested.
    // This does not do a version check.  The binder applies version policy
    // further up the stack once it gets a successful bind.
    //
    BOOL TestCandidateRefMatchesDef(AssemblyName *pRequestedAssemblyName,
                                    AssemblyName *pBoundAssemblyName,
                                    BOOL tpaListAssembly)
    {
        DWORD dwIncludeFlags = AssemblyName::INCLUDE_DEFAULT;

        if (!tpaListAssembly)
        {
            SString &culture = pRequestedAssemblyName->GetCulture();
            if (culture.IsEmpty() || culture.EqualsCaseInsensitive(g_BinderVariables->cultureNeutral))
            {
                dwIncludeFlags |= AssemblyName::EXCLUDE_CULTURE;
            }
        }
        
        if (pRequestedAssemblyName->GetArchitecture() != peNone)
        {
            dwIncludeFlags |= AssemblyName::INCLUDE_ARCHITECTURE;
        }

        BINDER_LOG_ASSEMBLY_NAME(W("pBoundAssemblyName"), pBoundAssemblyName);

        return pBoundAssemblyName->Equals(pRequestedAssemblyName, dwIncludeFlags);
    }
    
    HRESULT BindSatelliteResourceByResourceRoots(ApplicationContext  *pApplicationContext,
                                          StringArrayList     *pResourceRoots,
                                          AssemblyName        *pRequestedAssemblyName,
                                          BindResult          *pBindResult)
    {
        HRESULT hr = S_OK;

        SString &simpleNameRef = pRequestedAssemblyName->GetSimpleName();
        SString &cultureRef = pRequestedAssemblyName->GetCulture();
        
        _ASSERTE(!cultureRef.IsEmpty() && !cultureRef.EqualsCaseInsensitive(g_BinderVariables->cultureNeutral));
        
        for (UINT i = 0; i < pResourceRoots->GetCount(); i++)
        {
            ReleaseHolder<Assembly> pAssembly;
            SString &wszBindingPath = (*pResourceRoots)[i];
            SString fileName(wszBindingPath);

            CombinePath(fileName, cultureRef, fileName);
            CombinePath(fileName, simpleNameRef, fileName);
            fileName.Append(W(".dll"));

            hr = AssemblyBinder::GetAssembly(fileName,
                                             FALSE /* fIsInGAC */,
                                             FALSE /* fExplicitBindToNativeImage */,
                                             &pAssembly);

            // Missing files are okay and expected when probing
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            {
                continue;
            }
            
            IF_FAIL_GO(hr);

            AssemblyName *pBoundAssemblyName = pAssembly->GetAssemblyName();
            if (TestCandidateRefMatchesDef(pRequestedAssemblyName, pBoundAssemblyName, false /*tpaListAssembly*/))
            {
                pBindResult->SetResult(pAssembly);
                GO_WITH_HRESULT(S_OK);
            }

#ifdef FEATURE_VERSIONING_LOG
            // Log the candidates we throw out for diagnostics
            IF_FAIL_GO(LogConfigurationError(pApplicationContext,
                                             pRequestedAssemblyName,
                                             pBoundAssemblyName));
#endif // FEATURE_VERSIONING_LOG
            
            IF_FAIL_GO(FUSION_E_REF_DEF_MISMATCH);
        }

        // Up-stack expects S_OK when we don't find any candidate assemblies and no fatal error occurred (ie, no S_FALSE)
        hr = S_OK;
    Exit:
        return hr;
    }
    
    /*
     * BindByTpaList is the entry-point for the custom binding algorithm in CoreCLR.
     * Platform assemblies are specified as a list of files.  This list is the only set of
     * assemblies that we will load as platform.  They can be specified as IL or NIs.
     *
     * Resources for platform assemblies are located by probing starting at the Platform Resource Roots,
     * a set of folders configured by the host.
     *
     * If a requested assembly identity cannot be found in the TPA list or the resource roots,
     * it is considered an application assembly.  We probe for application assemblies in one of two
     * sets of paths: the AppNiPaths, a list of paths containing native images, and the AppPaths, a
     * list of paths containing IL files and satellite resource folders.
     *
     */
    /* static */
    HRESULT AssemblyBinder::BindByTpaList(ApplicationContext  *pApplicationContext,
                                          AssemblyName        *pRequestedAssemblyName,
                                          bool                 excludeAppPaths,
                                          BindResult          *pBindResult)
    {
        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("AssemblyBinder::BindByTpaList"));

        SString &culture = pRequestedAssemblyName->GetCulture();
        bool fPartialMatchOnTpa = false;
        
        if (!culture.IsEmpty() && !culture.EqualsCaseInsensitive(g_BinderVariables->cultureNeutral))
        {
            //
            // Satellite resource probing strategy is to look under each of the Platform Resource Roots
            // followed by App Paths.
            //
            
            hr = BindSatelliteResourceByResourceRoots(pApplicationContext, 
                                                      pApplicationContext->GetPlatformResourceRoots(), 
                                                      pRequestedAssemblyName, 
                                                      pBindResult);
            
            // We found a platform resource file with matching file name, but whose ref-def didn't match.  Fall
            // back to application resource lookup to handle case where a user creates resources with the same
            // names as platform ones.
            if (hr != FUSION_E_CONFIGURATION_ERROR)
            {
                IF_FAIL_GO(hr);
            }
            
            if (!pBindResult->HaveResult())
            {
                IF_FAIL_GO(BindSatelliteResourceByResourceRoots(pApplicationContext, 
                                                                pApplicationContext->GetAppPaths(), 
                                                                pRequestedAssemblyName, 
                                                                pBindResult));
            }
        }
        else
        {
            // Is assembly on TPA list?
            SString &simpleName = pRequestedAssemblyName->GetSimpleName();
            SimpleNameToFileNameMap * tpaMap = pApplicationContext->GetTpaList();
            const SimpleNameToFileNameMapEntry *pTpaEntry = tpaMap->LookupPtr(simpleName.GetUnicode());
            ReleaseHolder<Assembly> pTPAAssembly;
            if (pTpaEntry != nullptr)
            {
                if (pTpaEntry->m_wszNIFileName != nullptr)
                {
                    SString fileName(pTpaEntry->m_wszNIFileName);

                    hr = GetAssembly(fileName,
                                     TRUE,  // fIsInGAC
                                     TRUE,  // fExplicitBindToNativeImage
                                     &pTPAAssembly);
                }
                else
                {
                    _ASSERTE(pTpaEntry->m_wszILFileName != nullptr);
                    SString fileName(pTpaEntry->m_wszILFileName);
                    
                    hr = GetAssembly(fileName,
                                     TRUE,  // fIsInGAC
                                     FALSE, // fExplicitBindToNativeImage
                                     &pTPAAssembly);
                }

                // On file not found, simply fall back to app path probing
                if (hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                {
                    // Any other error is fatal
                    IF_FAIL_GO(hr);
                    
                    if (TestCandidateRefMatchesDef(pRequestedAssemblyName, pTPAAssembly->GetAssemblyName(), true /*tpaListAssembly*/))
                    {
                        // We have found the requested assembly match on TPA with validation of the full-qualified name. Bind to it.
                        pBindResult->SetResult(pTPAAssembly);
                        GO_WITH_HRESULT(S_OK);
                    }
                    else
                    {
                        // We found the assembly on TPA but it didn't match the RequestedAssembly assembly-name. In this case, lets proceed to see if we find the requested
                        // assembly in the App paths.
                        fPartialMatchOnTpa = true;
                    }
                }
                
                // We either didn't find a candidate, or the ref-def failed.  Either way; fall back to app path probing.
            }

            if (!excludeAppPaths)
            {
                // This loop executes twice max.  First time through we probe AppNiPaths, the second time we probe AppPaths
                bool parseAppNiPaths = true;
                for (;;)
                {
                    StringArrayList *pBindingPaths = parseAppNiPaths ? pApplicationContext->GetAppNiPaths() : pApplicationContext->GetAppPaths();

                    // Loop through the binding paths looking for a matching assembly
                    for (DWORD i = 0; i < pBindingPaths->GetCount(); i++)
                    {
                        ReleaseHolder<Assembly> pAssembly;
                        LPCWSTR wszBindingPath = (*pBindingPaths)[i];

                        SString &simpleName = pRequestedAssemblyName->GetSimpleName();

                        // Look for a matching dll first
                        hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

                        {
                            SString fileName(wszBindingPath);
                            CombinePath(fileName, simpleName, fileName);
                            if (parseAppNiPaths)
                            {
                                fileName.Append(W(".ni.dll"));
                                hr = GetAssembly(fileName,
                                                 FALSE, // fIsInGAC
                                                 TRUE,  // fExplicitBindToNativeImage
                                                 &pAssembly);
                            }
                            else
                            {
                                fileName.Append(W(".dll"));
                                hr = GetAssembly(fileName,
                                                 FALSE, // fIsInGAC
                                                 FALSE, // fExplicitBindToNativeImage
                                                 &pAssembly);
                            }
                        }

                        if (FAILED(hr))
                        {
                            SString fileName(wszBindingPath);
                            CombinePath(fileName, simpleName, fileName);

                            if (parseAppNiPaths)
                            {
                                fileName.Append(W(".ni.exe"));
                                hr = GetAssembly(fileName,
                                                 FALSE, // fIsInGAC
                                                 TRUE,  // fExplicitBindToNativeImage
                                                 &pAssembly);
                            }
                            else
                            {
                                fileName.Append(W(".exe"));
                                hr = GetAssembly(fileName,
                                                 FALSE, // fIsInGAC
                                                 FALSE, // fExplicitBindToNativeImage
                                                 &pAssembly);
                            }
                        }

                        // Since we're probing, file not founds are ok and we should just try another
                        // probing path
                        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                        {
                            continue;
                        }
                        IF_FAIL_GO(hr);

                        // We found a candidate.  
                        //
                        // Below this point, we either establish that the ref-def matches, or
                        // we fail the bind.

                        // Compare requested AssemblyName with that from the candidate assembly 
                        if (TestCandidateRefMatchesDef(pRequestedAssemblyName, pAssembly->GetAssemblyName(), false /*tpaListAssembly*/))
                        {
                            // At this point, we have found an assembly with the expected name in the App paths. If this was also found on TPA,
                            // make sure that the app assembly has the same fullname (excluding version) as the TPA version. If it does, then
                            // we should bind to the TPA assembly. If it does not, then bind to the app assembly since it has a different fullname than the 
                            // TPA assembly.
                            if (fPartialMatchOnTpa)
                            {
                                if (TestCandidateRefMatchesDef(pAssembly->GetAssemblyName(), pTPAAssembly->GetAssemblyName(), true /*tpaListAssembly*/))
                                {
                                    // Fullname (SimpleName+Culture+PKT) matched for TPA and app assembly - so bind to TPA instance.
                                    pBindResult->SetResult(pTPAAssembly);
                                    GO_WITH_HRESULT(S_OK);
                                }
                                else
                                {
                                    // Fullname (SimpleName+Culture+PKT) did not match for TPA and app assembly - so bind to app instance.
                                    pBindResult->SetResult(pAssembly);
                                    GO_WITH_HRESULT(S_OK);
                                }
                            }
                            else
                            {
                                // We didn't see this assembly on TPA - so simply bind to the app instance.
                                pBindResult->SetResult(pAssembly);
                                GO_WITH_HRESULT(S_OK);
                            }
                        }

#ifdef FEATURE_VERSIONING_LOG
                        // Log the candidates we throw out for diagnostics
                        IF_FAIL_GO(LogConfigurationError(pApplicationContext,
                            pRequestedAssemblyName,
                            pAssembly->GetAssemblyName()));
#endif // FEATURE_VERSIONING_LOG

                        IF_FAIL_GO(FUSION_E_REF_DEF_MISMATCH);

                    }

                    if (!parseAppNiPaths)
                    {
                        break;
                    }

                    parseAppNiPaths = false;
                }
            }
        }
        
        // Couldn't find a matching assembly in any of the probing paths
        // Return S_OK here.  BindByName will interpret a lack of BindResult
        // as a failure to find a matching assembly.  Other callers of this
        // function, such as BindLockedOrService will interpret as deciding
        // not to override an explicit bind with a probed assembly
        hr = S_OK;
        
    Exit:
        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::BindByTpaList"), hr);
        return hr;  
    }
    
    /* static */
    HRESULT AssemblyBinder::GetAssembly(SString     &assemblyPath,
                                        BOOL         fIsInGAC,
                                        
                                        // When binding to the native image, should we
                                        // assume assemblyPath explicitly specifies that
                                        // NI?  (If not, infer the path to the NI
                                        // implicitly.)
                                        BOOL         fExplicitBindToNativeImage,
                                        
                                        Assembly   **ppAssembly,

                                        // If assemblyPath refers to a native image without metadata,
                                        // szMDAssemblyPath gives the alternative file to get metadata.
                                        LPCTSTR      szMDAssemblyPath)
    {
        HRESULT hr = S_OK;

        BINDER_LOG_ENTER(W("Assembly::GetAssembly"));
        BINDER_LOG_STRING(W("assemblyPath"), assemblyPath);

        _ASSERTE(ppAssembly != NULL);

        ReleaseHolder<Assembly> pAssembly;
        ReleaseHolder<IMDInternalImport> pIMetaDataAssemblyImport;
        DWORD dwPAFlags[2];
        PEKIND PeKind = peNone;
        PEImage *pPEImage = NULL;
        PEImage *pNativePEImage = NULL;

        // Allocate assembly object
        SAFE_NEW(pAssembly, Assembly);

        // Obtain assembly meta data
        {
            LPCTSTR szAssemblyPath = const_cast<LPCTSTR>(assemblyPath.GetUnicode());

            BINDER_LOG_ENTER(W("BinderAcquirePEImage"));
            hr = BinderAcquirePEImage(szAssemblyPath, &pPEImage, &pNativePEImage, fExplicitBindToNativeImage);
            BINDER_LOG_LEAVE_HR(W("BinderAcquirePEImage"), hr);
            IF_FAIL_GO(hr);

            // If we found a native image, it might be an MSIL assembly masquerading as an native image
            // as a fallback mechanism for when the Triton tool chain wasn't able to generate a native image.
            // In that case it will not have a native header, so just treat it like the MSIL assembly it is.
            if (pNativePEImage)
            {
                BOOL hasHeader = TRUE;
                IF_FAIL_GO(BinderHasNativeHeader(pNativePEImage, &hasHeader));
                if (!hasHeader)
                {
                    BinderReleasePEImage(pPEImage);
                    BinderReleasePEImage(pNativePEImage);

                    BINDER_LOG_ENTER(W("BinderAcquirePEImageIL"));
                    hr = BinderAcquirePEImage(szAssemblyPath, &pPEImage, &pNativePEImage, false);
                    BINDER_LOG_LEAVE_HR(W("BinderAcquirePEImageIL"), hr);
                    IF_FAIL_GO(hr);
                }
            }

            BINDER_LOG_ENTER(W("BinderAcquireImport"));
            if (pNativePEImage)
                hr = BinderAcquireImport(pNativePEImage, &pIMetaDataAssemblyImport, dwPAFlags, TRUE);
            else
                hr = BinderAcquireImport(pPEImage, &pIMetaDataAssemblyImport, dwPAFlags, FALSE);

            BINDER_LOG_LEAVE_HR(W("BinderAcquireImport"), hr);
            IF_FAIL_GO(hr);

            if (pIMetaDataAssemblyImport == NULL && pNativePEImage != NULL)
            {
                // The native image doesn't contain metadata. Currently this is only supported for Windows.ni.winmd.
                // While loading Windows.winmd, CLRPrivBinderWinRT::GetAssemblyAndTryFindNativeImage should have passed
                // in a non-NULL szMDAssemblyPath, where we can load metadata from. If szMDAssemblyPath is NULL,
                // it indicates that the app is trying to load a non-WinMD assembly named Windows (it's possible
                // that the app happens to contain an assembly Windows.dll). To handle this case properly, we
                // return a file-not-found error, so the caller can continues it's search.
                if (szMDAssemblyPath == NULL)
                {
                    IF_FAIL_GO(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
                }
                else
                {
                    BINDER_LOG_ENTER(W("BinderAcquirePEImage"));
                    hr = BinderAcquirePEImage(szMDAssemblyPath, &pPEImage, NULL, FALSE);
                    BINDER_LOG_LEAVE_HR(W("BinderAcquirePEImage"), hr);
                    IF_FAIL_GO(hr);

                    BINDER_LOG_ENTER(W("BinderAcquireImport"));
                    hr = BinderAcquireImport(pPEImage, &pIMetaDataAssemblyImport, dwPAFlags, FALSE);
                    BINDER_LOG_LEAVE_HR(W("BinderAcquireImport"), hr);
                    IF_FAIL_GO(hr);
                }
            }

            IF_FAIL_GO(TranslatePEToArchitectureType(dwPAFlags, &PeKind));
        }

        // Initialize assembly object
        IF_FAIL_GO(pAssembly->Init(pIMetaDataAssemblyImport,
                                   PeKind,
                                   pPEImage,
                                   pNativePEImage,
                                   assemblyPath,
                                   fIsInGAC));

        // We're done
        *ppAssembly = pAssembly.Extract();

    Exit:

        BinderReleasePEImage(pPEImage);
        BinderReleasePEImage(pNativePEImage);
        
        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::GetAssembly"), hr);

        // Normalize file not found
        if ((FAILED(hr)) && IsFileNotFound(hr))
        {
            hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }

        return hr;
    }

#ifndef CROSSGEN_COMPILE

    /* static */
    HRESULT AssemblyBinder::Register(ApplicationContext *pApplicationContext,
                                     BindResult         *pBindResult)
    {
        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("AssemblyBinder::Register"));

        _ASSERTE(!pBindResult->GetIsContextBound());

        pApplicationContext->IncrementVersion();

        // Register the bindResult in the ExecutionContext only if we dont have it already.
        // This method is invoked under a lock (by its caller), so we are thread safe.
        ContextEntry *pContextEntry = NULL;
        hr = FindInExecutionContext(pApplicationContext, pBindResult->GetAssemblyName(), &pContextEntry);
        if (hr == S_OK)
        {
            if (pContextEntry == NULL)
            {
                ExecutionContext *pExecutionContext = pApplicationContext->GetExecutionContext();
                IF_FAIL_GO(pExecutionContext->Register(pBindResult));
            }
            else
            {
                // Update the BindResult with the contents of the ContextEntry we found
                pBindResult->SetResult(pContextEntry);
            }
        }

    Exit:
        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::Register"), hr);
        return hr;
    }

    /* static */
    HRESULT AssemblyBinder::RegisterAndGetHostChosen(ApplicationContext *pApplicationContext,
                                                     LONG                kContextVersion,
                                                     BindResult         *pBindResult,
                                                     BindResult         *pHostBindResult)
    {
        HRESULT hr = S_OK;
        BINDER_LOG_ENTER(W("AssemblyBinder::RegisterHostChosen"));

        _ASSERTE(pBindResult != NULL);
        _ASSERTE(pBindResult->HaveResult());
        _ASSERTE(pHostBindResult != NULL);

        if (!pBindResult->GetIsContextBound())
        {
            pHostBindResult->SetResult(pBindResult);

            {
                // Lock the application context
                CRITSEC_Holder contextLock(pApplicationContext->GetCriticalSectionCookie());

                // Only perform costly validation if other binds succeded before us
                if (kContextVersion != pApplicationContext->GetVersion())
                {
                    IF_FAIL_GO(AssemblyBinder::OtherBindInterfered(pApplicationContext,
                                                                   pBindResult));

                    if (hr == S_FALSE)
                    {
                        // Another bind interfered
                        GO_WITH_HRESULT(hr);
                    }
                }

                // No bind interfered, we can now register
                IF_FAIL_GO(Register(pApplicationContext,
                                    pHostBindResult));
            }
        }
        else
        {
            // No work required. Return the input
            pHostBindResult->SetResult(pBindResult);
        }

    Exit:
        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::RegisterHostChosen"), hr);
        return hr;
    }

    /* static */
    HRESULT AssemblyBinder::OtherBindInterfered(ApplicationContext *pApplicationContext,
                                                BindResult         *pBindResult)
    {
        HRESULT hr = S_FALSE;
        BINDER_LOG_ENTER(W("AssemblyBinder::OtherBindInterfered"));
        AssemblyName *pAssemblyName = pBindResult->GetAssemblyName();
        PathString assemblyDisplayName;

        _ASSERTE(pAssemblyName != NULL);
        
        // Look for already cached binding failure (ignore PA, every PA will lock the context)
        pAssemblyName->GetDisplayName(assemblyDisplayName, AssemblyName::INCLUDE_VERSION);
        hr = pApplicationContext->GetFailureCache()->Lookup(assemblyDisplayName);

        if (hr == S_OK)
        {
            ContextEntry *pContextEntry = NULL;

            hr = FindInExecutionContext(pApplicationContext, pAssemblyName, &pContextEntry);

            if ((hr == S_OK) && (pContextEntry == NULL))
            {
                // We can accept this bind in the domain
                GO_WITH_HRESULT(S_OK);
            }
        }

        // Some other bind interfered
        GO_WITH_HRESULT(S_FALSE);

    Exit:
        BINDER_LOG_LEAVE_HR(W("AssemblyBinder::OtherBindInterfered"), hr);
        return hr;
    }

#endif //CROSSGEN_COMPILE

#if !defined(DACCESS_COMPILE) && !defined(CROSSGEN_COMPILE)
HRESULT AssemblyBinder::BindUsingHostAssemblyResolver(/* in */ INT_PTR pManagedAssemblyLoadContextToBindWithin,
                                                      /* in */ AssemblyName       *pAssemblyName,
                                                      /* in */ IAssemblyName      *pIAssemblyName,
                                                      /* in */ CLRPrivBinderCoreCLR *pTPABinder,
                                                      /* out */ Assembly           **ppAssembly)
{
    HRESULT hr = E_FAIL;
    BINDER_LOG_ENTER(W("AssemblyBinder::BindUsingHostAssemblyResolver"));
    
    _ASSERTE(pManagedAssemblyLoadContextToBindWithin != NULL);
    
    // RuntimeInvokeHostAssemblyResolver will perform steps 2-4 of CLRPrivBinderAssemblyLoadContext::BindAssemblyByName.
    ICLRPrivAssembly *pLoadedAssembly = NULL;
    hr = RuntimeInvokeHostAssemblyResolver(pManagedAssemblyLoadContextToBindWithin, pIAssemblyName, 
                                           pTPABinder, pAssemblyName, &pLoadedAssembly);
    if (SUCCEEDED(hr))
    {
        _ASSERTE(pLoadedAssembly != NULL);
        *ppAssembly = static_cast<Assembly *>(pLoadedAssembly);
    }

    BINDER_LOG_LEAVE_HR(W("AssemblyBinder::BindUsingHostAssemblyResolver"), hr);
    return hr;
}

/* static */
HRESULT AssemblyBinder::BindUsingPEImage(/* in */  ApplicationContext *pApplicationContext,
                                         /* in */  BINDER_SPACE::AssemblyName *pAssemblyName,
                                         /* in */  PEImage            *pPEImage,
                                         /* in */  PEKIND              peKind,
                                         /* in */  IMDInternalImport  *pIMetaDataAssemblyImport, 
                                         /* [retval] [out] */  Assembly **ppAssembly)
{
    HRESULT hr = E_FAIL;
    BINDER_LOG_ENTER(W("AssemblyBinder::BindUsingPEImage"));
    
    // Indirect check that binder was initialized.
    _ASSERTE(g_BinderVariables != NULL);

    LONG kContextVersion = 0;
    BindResult bindResult;

    // Prepare binding data
    *ppAssembly = NULL;
    
    // Attempt the actual bind (eventually more than once)
Retry:
    {
        // Lock the application context
        CRITSEC_Holder contextLock(pApplicationContext->GetCriticalSectionCookie());
        
        // Attempt uncached bind and register stream if possible
        // We skip version compatibility check - so assemblies with same simple name will be reported
        // as a successfull bind. Below we compare MVIDs in that case instead (which is a more precise equality check).
        hr = BindByName(pApplicationContext,
                        pAssemblyName,
                        true,  // skipFailureCaching
                        true,  // skipVersionCompatibilityCheck
                        false, // excludeAppPaths
                        &bindResult);
        
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            IF_FAIL_GO(CreateImageAssembly(pIMetaDataAssemblyImport,
                                           peKind,
                                           pPEImage,
                                           NULL,
                                           &bindResult));
        }
        else if (hr == S_OK)
        {
            if (bindResult.HaveResult())
            {
                // Attempt was made to load an assembly that has the same name as a previously loaded one. Since same name
                // does not imply the same assembly, we will need to check the MVID to confirm it is the same assembly as being
                // requested.
                //
                GUID incomingMVID;
                ZeroMemory(&incomingMVID, sizeof(GUID));
                
                // If we cannot get MVID, then err on side of caution and fail the
                // load.
                IF_FAIL_GO(pIMetaDataAssemblyImport->GetScopeProps(NULL, &incomingMVID));
                
                GUID boundMVID;
                ZeroMemory(&boundMVID, sizeof(GUID));
                
                // If we cannot get MVID, then err on side of caution and fail the
                // load.
                IF_FAIL_GO(bindResult.GetAsAssembly()->GetMVID(&boundMVID));
                
                if (incomingMVID != boundMVID)
                {
                    // MVIDs do not match, so fail the load.
                    IF_FAIL_GO(COR_E_FILELOAD);
                }
                
                // MVIDs match - request came in for the same assembly that was previously loaded.
                // Let is through...
            }
        }
        
        // Remember the post-bind version of the context
        kContextVersion = pApplicationContext->GetVersion();

    } // lock(pApplicationContext)

    if (bindResult.HaveResult())
    {
        BindResult hostBindResult;

        // This has to happen outside the binder lock as it can cause new binds
        IF_FAIL_GO(RegisterAndGetHostChosen(pApplicationContext,
                                            kContextVersion,
                                            &bindResult,
                                            &hostBindResult));

        if (hr == S_FALSE)
        {
            // Another bind interfered. We need to retry entire bind.
            // This by design loops as long as needed because by construction we eventually
            // will succeed or fail the bind.
            bindResult.Reset();
            goto Retry;
        }
        else if (hr == S_OK)
        {
            *ppAssembly = hostBindResult.GetAsAssembly(TRUE /* fAddRef */);
        }
    }

Exit:
        
    BINDER_LOG_LEAVE_HR(W("AssemblyBinder::BindUsingPEImage"), hr);
    return hr;
}
#endif // !defined(DACCESS_COMPILE) && !defined(CROSSGEN_COMPILE)
};


