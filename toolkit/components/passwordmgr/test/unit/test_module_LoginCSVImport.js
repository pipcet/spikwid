/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Tests the LoginCSVImport module.
 */

"use strict";

const {
  LoginCSVImport,
  ImportFailedException,
  ImportFailedErrorType,
} = ChromeUtils.import("resource://gre/modules/LoginCSVImport.jsm");
const { LoginExport } = ChromeUtils.import(
  "resource://gre/modules/LoginExport.jsm"
);
const { TelemetryTestUtils: TTU } = ChromeUtils.import(
  "resource://testing-common/TelemetryTestUtils.jsm"
);

// Enable the collection (during test) for all products so even products
// that don't collect the data will be able to run the test without failure.
Services.prefs.setBoolPref(
  "toolkit.telemetry.testing.overrideProductsCheck",
  true
);

/**
 * Given an array of strings it creates a temporary CSV file that has them as content.
 *
 * @param {string[]} csvLines
 *        The lines that make up the CSV file.
 * @param {string} extension
 *        Optional parameter. Either 'csv' or 'tsv'. Default is 'csv'.
 * @returns {string} The path to the CSV file that was created.
 */
async function setupCsv(csvLines, extension) {
  // Cleanup state.
  TTU.getAndClearKeyedHistogram("FX_MIGRATION_LOGINS_QUANTITY");
  TTU.getAndClearKeyedHistogram("FX_MIGRATION_LOGINS_IMPORT_MS");
  TTU.getAndClearKeyedHistogram("FX_MIGRATION_LOGINS_JANK_MS");
  Services.logins.removeAllUserFacingLogins();

  let tmpFile = await LoginTestUtils.file.setupCsvFileWithLines(
    csvLines,
    extension
  );
  return tmpFile.path;
}

function checkMetaInfo(
  actual,
  expected,
  props = ["timesUsed", "timeCreated", "timePasswordChanged", "timeLastUsed"]
) {
  for (let prop of props) {
    // This will throw if not equal.
    equal(actual[prop], expected[prop], `Check ${prop}`);
  }
  return true;
}

function checkLoginNewlyCreated(login) {
  // These will throw if not equal.
  LoginTestUtils.assertTimeIsAboutNow(login.timeCreated);
  LoginTestUtils.assertTimeIsAboutNow(login.timePasswordChanged);
  LoginTestUtils.assertTimeIsAboutNow(login.timeLastUsed);
  return true;
}

/**
 * Ensure that an import works with TSV.
 */
add_task(async function test_import_tsv() {
  let tsvFilePath = await setupCsv(
    [
      "url\tusername\tpassword\thttpRealm\tformActionOrigin\tguid\ttimeCreated\ttimeLastUsed\ttimePasswordChanged",
      `https://example.com:8080\tjoe@example.com\tqwerty\tMy realm\t""\t{5ec0d12f-e194-4279-ae1b-d7d281bb46f0}\t1589617814635\t1589710449871\t1589617846802`,
    ],
    "tsv"
  );

  await LoginCSVImport.importFromCSV(tsvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.authLogin({
        formActionOrigin: null,
        guid: "{5ec0d12f-e194-4279-ae1b-d7d281bb46f0}",
        httpRealm: "My realm",
        origin: "https://example.com:8080",
        password: "qwerty",
        passwordField: "",
        timeCreated: 1589617814635,
        timeLastUsed: 1589710449871,
        timePasswordChanged: 1589617846802,
        timesUsed: 1,
        username: "joe@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new login was added with the correct fields",
    (a, e) => a.equals(e) && checkMetaInfo(a, e)
  );
});

/**
 * Ensure that an import fails if there is no username column in a TSV file.
 */
add_task(async function test_import_tsv_with_missing_columns() {
  let csvFilePath = await setupCsv(
    [
      "url\tusernameTypo\tpassword\thttpRealm\tformActionOrigin\tguid\ttimeCreated\ttimeLastUsed\ttimePasswordChanged",
      "https://example.com\tkramer@example.com\tqwerty\tMy realm\t\t{5ec0d12f-e194-4279-ae1b-d7d281bb46f7}\t1589617814635\t1589710449871\t1589617846802",
    ],
    "tsv"
  );

  await Assert.rejects(
    LoginCSVImport.importFromCSV(csvFilePath),
    /FILE_FORMAT_ERROR/,
    "Ensure missing username throws"
  );

  LoginTestUtils.checkLogins(
    [],
    "Check that no login was added without finding columns"
  );
});

/**
 * Ensure that an import fails if there is no username column. We don't want
 * to accidentally import duplicates due to a name mismatch for the username column.
 */
add_task(async function test_import_lacking_username_column() {
  let csvFilePath = await setupCsv([
    "url,usernameTypo,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    `https://example.com,joe@example.com,qwerty,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb46f0},1589617814635,1589710449871,1589617846802`,
  ]);

  await Assert.rejects(
    LoginCSVImport.importFromCSV(csvFilePath),
    /FILE_FORMAT_ERROR/,
    "Ensure missing username throws"
  );

  LoginTestUtils.checkLogins(
    [],
    "Check that no login was added without finding a username column"
  );
});

/**
 * Ensure that an import fails if there are two headings that map to one login field.
 */
add_task(async function test_import_with_duplicate_columns() {
  // Two origin columns (url & login_uri).
  // One row has different values and the other has the same.
  let csvFilePath = await setupCsv([
    "url,login_uri,username,login_password",
    "https://example.com/path,https://example.com,john@example.com,azerty",
    "https://mozilla.org,https://mozilla.org,jdoe@example.com,qwerty",
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.formLogin({
        formActionOrigin: "",
        httpRealm: null,
        origin: "https://mozilla.org",
        password: "qwerty",
        passwordField: "",
        timesUsed: 1,
        username: "jdoe@example.com",
        usernameField: "",
      }),
    ],
    "Check that no login was added with duplicate columns of differing values"
  );
});

/**
 * Ensure that import is allowed with only origin, username, password and that
 * one can mix and match column naming between conventions from different
 * password managers (so that we better support new/unknown password managers).
 */
add_task(async function test_import_minimal_with_mixed_naming() {
  let csvFilePath = await setupCsv([
    "url,username,login_password",
    "ftp://example.com,john@example.com,azerty",
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);
  LoginTestUtils.checkLogins(
    [
      TestData.formLogin({
        formActionOrigin: "",
        httpRealm: null,
        origin: "ftp://example.com",
        password: "azerty",
        passwordField: "",
        timesUsed: 1,
        username: "john@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new login was added with the correct fields",
    (a, e) =>
      a.equals(e) &&
      checkMetaInfo(a, e, ["timesUsed"]) &&
      checkLoginNewlyCreated(a)
  );
});

/**
 * Imports login data from the latest Firefox CSV file for various logins from
 * LoginTestUtils.testData.loginList().
 */
add_task(async function test_import_from_firefox_various_latest() {
  await setupCsv([]);
  info("Populate the login list for export");
  let logins = LoginTestUtils.testData.loginList();
  for (let loginInfo of logins) {
    Services.logins.addLogin(loginInfo);
  }

  let tmpFilePath = FileTestUtils.getTempFile("logins.csv").path;
  await LoginExport.exportAsCSV(tmpFilePath);

  await LoginCSVImport.importFromCSV(tmpFilePath);

  LoginTestUtils.checkLogins(
    logins,
    "Check that all of LoginTestUtils.testData.loginList can be re-imported"
  );
});

/**
 * Imports login data from a Firefox CSV file without quotes.
 */
add_task(async function test_import_from_firefox_auth() {
  let csvFilePath = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    `https://example.com:8080,joe@example.com,qwerty,My realm,"",{5ec0d12f-e194-4279-ae1b-d7d281bb46f0},1589617814635,1589710449871,1589617846802`,
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.authLogin({
        formActionOrigin: null,
        guid: "{5ec0d12f-e194-4279-ae1b-d7d281bb46f0}",
        httpRealm: "My realm",
        origin: "https://example.com:8080",
        password: "qwerty",
        passwordField: "",
        timeCreated: 1589617814635,
        timeLastUsed: 1589710449871,
        timePasswordChanged: 1589617846802,
        timesUsed: 1,
        username: "joe@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new login was added with the correct fields",
    (a, e) => a.equals(e) && checkMetaInfo(a, e)
  );
});

/**
 * Imports login data from a Firefox CSV file with quotes.
 */
add_task(async function test_import_from_firefox_auth_with_quotes() {
  let csvFilePath = await setupCsv([
    '"url","username","password","httpRealm","formActionOrigin","guid","timeCreated","timeLastUsed","timePasswordChanged"',
    '"https://example.com","joe@example.com","qwerty2","My realm",,"{5ec0d12f-e194-4279-ae1b-d7d281bb46f0}","1589617814635","1589710449871","1589617846802"',
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.authLogin({
        formActionOrigin: null,
        httpRealm: "My realm",
        origin: "https://example.com",
        password: "qwerty2",
        passwordField: "",
        timeCreated: 1589617814635,
        timeLastUsed: 1589710449871,
        timePasswordChanged: 1589617846802,
        timesUsed: 1,
        username: "joe@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new login was added with the correct fields",
    (a, e) => a.equals(e) && checkMetaInfo(a, e)
  );
});

/**
 * Imports login data from a Firefox CSV file where only cells containing a comma are quoted.
 */
add_task(async function test_import_from_firefox_auth_some_quoted_fields() {
  let csvFilePath = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    'https://example.com,joe@example.com,"one,two,tree","My realm",,{5ec0d12f-e194-4279-ae1b-d7d281bb46f0},1589617814635,1589710449871,1589617846802',
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.authLogin({
        formActionOrigin: null,
        httpRealm: "My realm",
        origin: "https://example.com",
        password: "one,two,tree",
        passwordField: "",
        timeCreated: 1589617814635,
        timePasswordChanged: 1589617846802,
        timeLastUsed: 1589710449871,
        timesUsed: 1,
        username: "joe@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new login was added with the correct fields",
    (a, e) => a.equals(e) && checkMetaInfo(a, e)
  );
});

/**
 * Imports login data from a Firefox CSV file with an empty formActionOrigin and null httpRealm
 */
add_task(async function test_import_from_firefox_form_empty_formActionOrigin() {
  let csvFilePath = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "https://example.com,joe@example.com,s3cret1,,,{5ec0d12f-e194-4279-ae1b-d7d281bb46f0},1589617814636,1589710449872,1589617846803",
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.formLogin({
        formActionOrigin: "",
        httpRealm: null,
        origin: "https://example.com",
        password: "s3cret1",
        passwordField: "",
        timeCreated: 1589617814636,
        timePasswordChanged: 1589617846803,
        timeLastUsed: 1589710449872,
        timesUsed: 1,
        username: "joe@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new login was added with the correct fields",
    (a, e) => a.equals(e) && checkMetaInfo(a, e)
  );
});

/**
 * Imports login data from a Firefox CSV file with a non-empty formActionOrigin and null httpRealm.
 */
add_task(async function test_import_from_firefox_form_with_formActionOrigin() {
  let csvFilePath = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "http://example.com,joe@example.com,s3cret1,,https://other.example.com,{5ec0d12f-e194-4279-ae1b-d7d281bb46f1},1589617814635,1589710449871,1589617846802",
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.formLogin({
        formActionOrigin: "https://other.example.com",
        httpRealm: null,
        origin: "http://example.com",
        password: "s3cret1",
        passwordField: "",
        timeCreated: 1589617814635,
        timePasswordChanged: 1589617846802,
        timeLastUsed: 1589710449871,
        timesUsed: 1,
        username: "joe@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new login was added with the correct fields",
    (a, e) => a.equals(e) && checkMetaInfo(a, e)
  );
});

/**
 * Imports login data from a Bitwarden CSV file.
 * `name` is ignored until bug 1433770.
 */
add_task(async function test_import_from_bitwarden_csv() {
  let csvFilePath = await setupCsv([
    "folder,favorite,type,name,notes,fields,login_uri,login_username,login_password,login_totp",
    `,,note,jane's note,"secret note, ignore me!",,,,,`,
    ",,login,example.com,,,https://example.com/login,jane@example.com,secret_password",
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.formLogin({
        formActionOrigin: "",
        httpRealm: null,
        origin: "https://example.com",
        password: "secret_password",
        passwordField: "",
        timesUsed: 1,
        username: "jane@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new Bitwarden login was added with the correct fields",
    (a, e) =>
      a.equals(e) &&
      checkMetaInfo(a, e, ["timesUsed"]) &&
      checkLoginNewlyCreated(a)
  );
});

/**
 * Imports login data from a Chrome CSV file.
 * `name` is ignored until bug 1433770.
 */
add_task(async function test_import_from_chrome_csv() {
  let csvFilePath = await setupCsv([
    "name,url,username,password",
    "example.com,https://example.com/login,jane@example.com,secret_chrome_password",
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.formLogin({
        formActionOrigin: "",
        httpRealm: null,
        origin: "https://example.com",
        password: "secret_chrome_password",
        passwordField: "",
        timesUsed: 1,
        username: "jane@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new Chrome login was added with the correct fields",
    (a, e) =>
      a.equals(e) &&
      checkMetaInfo(a, e, ["timesUsed"]) &&
      checkLoginNewlyCreated(a)
  );
});

/**
 * Imports login data from a KeepassXC CSV file.
 * `Title` is ignored until bug 1433770.
 */
add_task(async function test_import_from_keepassxc_csv() {
  let csvFilePath = await setupCsv([
    `"Group","Title","Username","Password","URL","Notes"`,
    `"NewDatabase/Internet","Amazing","test@example.com","<password>","https://example.org",""`,
  ]);

  await LoginCSVImport.importFromCSV(csvFilePath);

  LoginTestUtils.checkLogins(
    [
      TestData.formLogin({
        formActionOrigin: "",
        httpRealm: null,
        origin: "https://example.org",
        password: "<password>",
        passwordField: "",
        timesUsed: 1,
        username: "test@example.com",
        usernameField: "",
      }),
    ],
    "Check that a new KeepassXC login was added with the correct fields",
    (a, e) =>
      a.equals(e) &&
      checkMetaInfo(a, e, ["timesUsed"]) &&
      checkLoginNewlyCreated(a)
  );
});

/**
 * Imports login data summary contains added logins.
 */
add_task(async function test_import_summary_contains_added_login() {
  let csvFilePath = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "https://added.example.com,jane@example.com,added_passwordd,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb0003},1589617814635,1589710449871,1589617846802",
  ]);

  let [added] = await LoginCSVImport.importFromCSV(csvFilePath);

  equal(added.result, "added", `Check that the login was added`);
});

/**
 * Imports login data summary contains modified logins.
 */
add_task(async function test_import_summary_contains_modified_login() {
  let initialDataFile = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "https://modifiedwithguid.example.com,jane@example.com,initial_password,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb0001},1589617814635,1589710449871,1589617846802",
    "https://modifiedwithoutguid.example.com,jane@example.com,initial_password,My realm,,,1589617814635,1589710449871,1589617846802",
  ]);
  await LoginCSVImport.importFromCSV(initialDataFile);

  let csvFile = await LoginTestUtils.file.setupCsvFileWithLines([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "https://modified.example.com,jane@example.com,modified_password,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb0001},1589617814635,1589710449871,1589617846999",
    "https://modifiedwithoutguid.example.com,jane@example.com,modified_password,My realm,,,1589617814635,1589710449871,1589617846999",
  ]);

  let [
    modifiedWithGuid,
    modifiedWithoutGuid,
  ] = await LoginCSVImport.importFromCSV(csvFile.path);

  equal(
    modifiedWithGuid.result,
    "modified",
    `Check that the login was modified when it had the same guid`
  );
  equal(
    modifiedWithoutGuid.result,
    "modified",
    `Check that the login was modified when there was no guid data`
  );
});

/**
 * Imports login data summary contains unchanged logins.
 */
add_task(async function test_import_summary_contains_unchanged_login() {
  let initialDataFile = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "https://nochange.example.com,jane@example.com,nochange_password,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb0002},1589617814635,1589710449871,1589617846802",
  ]);
  await LoginCSVImport.importFromCSV(initialDataFile);

  let csvFile = await LoginTestUtils.file.setupCsvFileWithLines([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "https://nochange.example.com,jane@example.com,nochange_password,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb0002},1589617814635,1589710449871,1589617846802",
  ]);

  let [noChange] = await LoginCSVImport.importFromCSV(csvFile.path);

  equal(noChange.result, "no_change", `Check that the login was not changed`);
});

/**
 * Imports login data summary contains logins with errors.
 */
add_task(async function test_import_summary_contains_logins_with_errors() {
  let csvFilePath = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "https://invalid.password.example.com,jane@example.com,,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb0002},1589617814635,1589710449871,1589617846802",
    ",jane@example.com,invalid_origin,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb0005},1589617814635,1589710449871,1589617846802",
  ]);
  let [invalidPassword, invalidOrigin] = await LoginCSVImport.importFromCSV(
    csvFilePath
  );

  equal(
    invalidPassword.result,
    "error_invalid_password",
    `Check that the invalid password error is reported`
  );
  equal(
    invalidOrigin.result,
    "error_invalid_origin",
    `Check that the invalid origin error is reported`
  );
});

/**
 * Imports login with wrong file format will have correct errorType.
 */
add_task(async function test_import_summary_with_bad_format() {
  let csvFilePath = await setupCsv(["password", "123qwe!@#QWE"]);

  await Assert.rejects(
    LoginCSVImport.importFromCSV(csvFilePath),
    /FILE_FORMAT_ERROR/,
    "Check that the errorType is file format error"
  );

  LoginTestUtils.checkLogins(
    [],
    "Check that no login was added with bad format"
  );
});

/**
 * Imports login with wrong file type will have correct errorType.
 */
add_task(async function test_import_summary_with_non_csv_file() {
  let csvFilePath = await setupCsv([
    "<body>this is totally not a csv file</body>",
  ]);

  await Assert.rejects(
    LoginCSVImport.importFromCSV(csvFilePath),
    /FILE_FORMAT_ERROR/,
    "Check that the errorType is file format error"
  );

  LoginTestUtils.checkLogins(
    [],
    "Check that no login was added with file of different format"
  );
});

/**
 * Imports login with wrong file type will have correct errorType.
 */
add_task(async function test_import_summary_with_url_user_multiple_values() {
  let csvFilePath = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "https://example.com,jane@example.com,password1,My realm",
    "https://example.com,jane@example.com,password2,My realm",
  ]);

  let errorType;
  try {
    await LoginCSVImport.importFromCSV(csvFilePath);
  } catch (e) {
    if (e instanceof ImportFailedException) {
      errorType = e.errorType;
    }
  }

  equal(
    errorType,
    ImportFailedErrorType.CONFLICTING_VALUES_ERROR,
    `Check that the errorType is file format error in case of duplicate entries`
  );
}).skip(); // TODO: Bug 1687852, resolve duplicates when importing

/**
 * Imports login with wrong file type will have correct errorType.
 */
add_task(async function test_import_summary_with_multiple_guid_values() {
  let csvFilePath = await setupCsv([
    "url,username,password,httpRealm,formActionOrigin,guid,timeCreated,timeLastUsed,timePasswordChanged",
    "https://example1.com,jane1@example.com,password1,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb0004},1589617814635,1589710449871,1589617846802",
    "https://example2.com,jane2@example.com,password2,My realm,,{5ec0d12f-e194-4279-ae1b-d7d281bb0004},1589617814635,1589710449871,1589617846802",
  ]);

  let errorType;
  try {
    await LoginCSVImport.importFromCSV(csvFilePath);
  } catch (e) {
    if (e instanceof ImportFailedException) {
      errorType = e.errorType;
    }
  }

  equal(
    errorType,
    ImportFailedErrorType.CONFLICTING_VALUES_ERROR,
    `Check that the errorType is file format error in case of duplicate entries`
  );
});
