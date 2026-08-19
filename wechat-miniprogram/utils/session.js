const auth = require('./auth');
const { createDav } = require('./dav');
const { wxRequest, wxUploadFile } = require('./wxreq');

function session() {
  return getApp().getSession();
}

function authHeader() {
  const s = session();
  return 'Basic ' + auth.base64Encode(s.user + ':' + s.pass);
}

function getDav() {
  return createDav({
    baseUrl: session().baseUrl,
    authHeader: authHeader(),
    request: wxRequest,
    uploadFile: wxUploadFile
  });
}

module.exports = { session, authHeader, getDav };
