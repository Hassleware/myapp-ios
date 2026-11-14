// HTTPS on the iPhone, through Apple's NSURLSession. net.cpp has already checked the address, host and headers.
// iOS itself refuses plain http and bad certificates (App Transport Security); nothing here turns that off.
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>

// Redirects are not followed: one could carry a login header to a host the game never listed.
@interface GameLabNoRedirect : NSObject <NSURLSessionTaskDelegate>
@end

@implementation GameLabNoRedirect
- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
    completionHandler(nil);
}
@end

// Waits for the answer, so it is only ever called from net.cpp's request threads.
extern "C" int gamelab_http(const char* method, const char* url, const char* const* headers, int headerCount,
                            const char* body, size_t bodySize, double timeout, size_t maxBytes,
                            char** data, size_t* size, char* error, size_t errorSize) {
    static NSURLSession* session;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // Nothing is remembered between requests or written to disk: no cookies, cache or saved logins.
        NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.HTTPCookieStorage = nil;
        config.URLCache = nil;
        config.URLCredentialStorage = nil;
        session = [NSURLSession sessionWithConfiguration:config delegate:[[GameLabNoRedirect alloc] init] delegateQueue:nil];
    });

    NSString* address = [NSString stringWithUTF8String:url];
    NSURL* target = address ? [NSURL URLWithString:address] : nil;
    if (!target) {
        strlcpy(error, "the address could not be read", errorSize);
        return 0;
    }
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:target
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:timeout];
    request.HTTPMethod = [NSString stringWithUTF8String:method];
    for (int i = 0; i < headerCount; i++) {
        NSString* name = [NSString stringWithUTF8String:headers[i * 2]];
        NSString* value = [NSString stringWithUTF8String:headers[i * 2 + 1]];
        if (name && value) [request setValue:value forHTTPHeaderField:name];
    }
    if (bodySize) request.HTTPBody = [NSData dataWithBytes:body length:bodySize];

    __block int status = 0;
    __block NSData* received = nil;
    __block NSString* problem = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSURLSessionDataTask* task = [session dataTaskWithRequest:request
                                            completionHandler:^(NSData* got, NSURLResponse* response, NSError* failure) {
        if (failure) problem = failure.localizedDescription;
        else if ([response isKindOfClass:[NSHTTPURLResponse class]]) status = (int)((NSHTTPURLResponse*)response).statusCode;
        received = got;
        dispatch_semaphore_signal(done);
    }];
    [task resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);

    // ponytail: the size limit is checked after the download. Use a data delegate to cut it off early if that matters.
    if (!problem && received.length > maxBytes) problem = @"the answer is over 4 MB";
    if (problem || status == 0) {
        strlcpy(error, problem ? problem.UTF8String : "the request failed", errorSize);
        return 0;
    }
    *size = received.length;
    *data = (char*)malloc(*size + 1);
    if (*size) memcpy(*data, received.bytes, *size);
    return status;
}
